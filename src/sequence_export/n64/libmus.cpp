// SPDX-License-Identifier: MIT

#include "ultralib/sequence_export/n64/libmus.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "seqlib/midi/recorder.h"
#include "xsflib/binary.h"
#include "xsflib/bytes.h"

#include "libmus_shared.h"
#include "n64/mus_player_common.h"
#include "rsp_audio.h"

namespace ultralib::sequence_export::n64
{

namespace
{

namespace midi = seqlib::midi;
using libmus::kHighestVoiceVolume;
using libmus::kKeyOfNote;
using libmus::kTopSteps;
using libmus::kVolumeShift;
using ultralib::n64::mus_player_common::kDefaultBendRange;
using ultralib::n64::mus_player_common::kDrumSize;
using ultralib::n64::mus_player_common::kEndlessLength;
using ultralib::n64::mus_player_common::kEnvelopeRelease;
using ultralib::n64::mus_player_common::kEnvelopeSize;
using ultralib::n64::mus_player_common::kForever;
using ultralib::n64::mus_player_common::kMostCommandsInAStep;
using ultralib::n64::mus_player_common::kRest;
using ultralib::n64::mus_player_common::kVibratoUnitsPerSemitone;
using enum ultralib::n64::mus_player_common::Command;

// The loops that a channel can have open at once.
constexpr auto kForDepth = static_cast<std::size_t>(ultralib::n64::MusChannel::kForDepth);

// The scale of a song's tempo that changes nothing.
constexpr int kFullTempoScale = 128;

// The player's callbacks a second. The tempos of the player count callbacks. The player asks for its next callback in
// this many microseconds. The synthesizer rounds that to whole samples.
constexpr int kCallbacksPerSecond = 60;
constexpr double kCallbackMicroseconds = 1000000 / kCallbacksPerSecond;

// A channel's clock counts this much for a tick.
constexpr int kFramesPerTick = 256;

// A script's tempo before the song sets one.
constexpr int kDefaultTempo = 96 * kFramesPerTick / kCallbacksPerSecond;

// The file's ticks in a quarter note. The tempo gives their time.
constexpr uint32_t kTicksPerQuarter = 48;

// The highest ratio of a wave's rate to the output's. A higher note is silent.
constexpr double kHighestSemitones = 12.0;

// A voice's volume stops at its top while volume * envelope * velocity * song volume is this or more.
constexpr int64_t kFullVoiceProduct = int64_t{kHighestVoiceVolume + 1} << kVolumeShift;

// What's sent to each song's channels. The player shares its 16 MIDI tracks among them.
constexpr std::size_t kMidiTracks = 16;

constexpr double kPi = 3.14159260000000007;

// A main loop can't end later than this. The longest performance ends long before.
constexpr uint64_t kLongestLoopEnd = 200000000;

// The highest of an envelope's three levels.
int TopOfEnvelope(const std::array<uint8_t, kEnvelopeSize>& envelope)
{
    return std::max({envelope[1], envelope[3], envelope[5]});
}

int TempoOfScript(int value)
{
    return value * kFramesPerTick * 96 / 120 / kCallbacksPerSecond;
}

double TicksPerSecond(int tempo, uint32_t output_rate)
{
    const double samples = std::floor(kCallbackMicroseconds * output_rate / 1000000.0 + 0.5);

    return tempo * (output_rate / samples) / kFramesPerTick;
}

// The bytes that a song's header points to. A song of the library's early form has no master track, and its channels
// name the bank's waves directly.
struct SongFile
{
    std::span<const uint8_t> bytes;
    bool early = false;
    uint32_t channels = 0;
    uint32_t waves = 0;
    uint32_t data_list = 0;
    uint32_t volume_list = 0;
    uint32_t bend_list = 0;
    uint32_t envelopes = 0;
    uint32_t drums = 0;
    uint32_t wave_table = 0;
    uint32_t master_track = 0;

    static SongFile Parse(std::span<const uint8_t> bytes, bool early)
    {
        constexpr std::size_t kHeaderSize = 0x2C;
        constexpr std::size_t kEarlyHeaderSize = 0x18;
        if (bytes.size() < (early ? kEarlyHeaderSize : kHeaderSize))
        {
            throw xsflib::FormatError("a song is too short for its header");
        }

        const auto word = [&](std::size_t at)
        {
            return xsflib::LoadBe32(bytes.data() + at);
        };

        SongFile file;
        file.bytes = bytes;
        file.early = early;
        if (early)
        {
            file.channels = word(0x00);
            file.data_list = word(0x04);
            file.volume_list = word(0x08);
            file.bend_list = word(0x0C);
            file.envelopes = word(0x10);
            file.drums = word(0x14);
        }
        else
        {
            file.channels = word(0x04);
            file.waves = word(0x08);
            file.data_list = word(0x0C);
            file.volume_list = word(0x10);
            file.bend_list = word(0x14);
            file.envelopes = word(0x18);
            file.drums = word(0x1C);
            file.wave_table = word(0x20);
            file.master_track = word(0x24);
        }

        if (file.data_list + std::size_t{4} * file.channels > bytes.size() ||
            file.volume_list + std::size_t{4} * file.channels > bytes.size() ||
            file.bend_list + std::size_t{4} * file.channels > bytes.size())
        {
            throw xsflib::FormatError("a song's lists don't fit in its file");
        }

        return file;
    }

    uint32_t Entry(uint32_t list, uint32_t channel) const
    {
        return xsflib::LoadBe32(bytes.data() + list + std::size_t{4} * channel);
    }

    int WaveOf(int wave) const
    {
        if (early)
        {
            return wave;
        }

        const std::size_t at = wave_table + std::size_t{2} * static_cast<std::size_t>(wave);
        if (at + 2 > bytes.size())
        {
            throw xsflib::FormatError("a song's wave is outside its table");
        }

        const uint16_t value = xsflib::LoadBe16(bytes.data() + at);
        return value == 0xFFFF ? -1 : value;
    }

    uint8_t At(std::size_t place) const
    {
        if (place >= bytes.size())
        {
            throw xsflib::FormatError("a song's list runs past its file");
        }

        return bytes[place];
    }
};

// A setting or a sound of a channel, at its tick.
struct Action
{
    enum class Kind : uint8_t
    {
        kOff,
        kVolume,
        kBend,
        kNote,
    };

    friend bool operator==(const Action&, const Action&) = default;

    uint32_t tick = 0;
    Kind kind = Kind::kNote;
    uint32_t note = 0; // kOff and kNote: the note's number among the channel's
    uint8_t key = 0;
    uint8_t velocity = 0; // kVolume: the volume
    uint8_t pan = 64;
    uint8_t reverb = 0;
    float semitones = 0.0F;
    LibmusPresetKey preset;
};

// The place of an envelope's first byte.
constexpr std::size_t kEnvelopeFirst = 1;

// How a channel's list goes: the lists hold the settings that a note takes when it starts.
class ChannelReader
{
public:
    ChannelReader(const SongFile& file, std::span<const double> detunes, const LibmusSettings& settings, uint32_t data,
                  uint32_t volume, uint32_t bend, midi::Recorder& recorder)
        : file_(file),
          detunes_(detunes),
          settings_(settings),
          place_(data),
          base_(data),
          volume_list_(volume),
          bend_list_(bend),
          volume_base_(volume),
          bend_base_(bend),
          recorder_(recorder)
    {
        // A channel of the early form starts without velocities (the code at 0x80055874 in Gex 64).
        if (file_.early)
        {
            velocity_on_ = false;
        }

        if (settings_.early_player)
        {
            envelope_[kEnvelopeRelease] = 1;
        }
    }

    // Runs the channel's list up to tick `end`.
    void Run(uint32_t end);

    // Runs the channel's list up to the end of its first endless loop, or its stop.
    void RunToLoop();

    const std::vector<Action>& Actions() const
    {
        return actions_;
    }

    const std::vector<std::pair<uint32_t, int>>& Tempos() const
    {
        return tempos_;
    }

    std::optional<uint32_t> LoopStart() const
    {
        return loop_start_;
    }

    std::optional<uint32_t> LoopEnd() const
    {
        return loop_end_;
    }

    uint32_t EndTick() const
    {
        return note_end_;
    }

private:
    struct OpenFor
    {
        uint8_t count = 0;
        std::size_t place = 0;
        std::size_t volume_place = 0;
        std::size_t bend_place = 0;
        int volume = 0;
        double bend = 0.0;
        int volume_repeat = 0;
        int bend_repeat = 0;
    };

    // A note that started in this tick. The stop of its preset is set once the tick has read the volume list.
    struct ClippedNote
    {
        std::size_t action = 0;
        int velocity = 0;
    };

    void Step();
    bool RunCommand(uint8_t command);
    void StartNote(int note);
    void EndCurrentNote(uint32_t tick);
    void ReadVolume();
    void ClipStartedNote();
    void ReadBend();
    void SendChanges(uint32_t tick);
    double PitchOfNoteAt(uint32_t tick);
    void SetEnvelope(std::size_t at);
    int Sized();

    const SongFile& file_;
    std::span<const double> detunes_;
    const LibmusSettings& settings_;
    std::size_t place_ = 0;
    std::size_t base_ = 0;
    std::size_t volume_list_ = 0;
    std::size_t bend_list_ = 0;
    std::size_t volume_base_ = 0;
    std::size_t bend_base_ = 0;
    midi::Recorder& recorder_;

    bool stopped_ = false;
    bool stop_at_loop_ = false;
    std::vector<OpenFor> fors_;
    std::optional<uint32_t> loop_start_;
    std::optional<uint32_t> loop_end_;

    uint32_t note_end_ = 0;
    int length_ = 1;
    int fixed_length_ = 0;
    bool ignore_length_ = false;
    bool velocity_on_ = true;
    int default_velocity_ = 127;
    int velocity_ = 0;
    int wave_ = 0;
    std::optional<uint32_t> drums_;
    int pan_ = 64;
    int transpose_ = 0;
    bool ignore_transpose_ = false;
    std::array<uint8_t, kEnvelopeSize> envelope_ = {1, 0, 1, 127, 255, 127, 15};
    int cutoff_ = 0;
    int end_it_ = 0;
    int volume_ = 127;
    int volume_repeat_ = 1;
    int bend_repeat_ = 1;
    double bend_ = 0.0;
    double bend_range_ = kDefaultBendRange;
    double bend_semitones_ = 0.0;
    int reverb_ = 0;
    int vibrato_delay_ = 0;
    int vibrato_speed_ = 0;
    double vibrato_amount_ = 0.0;
    double vibrato_ = 0.0;
    int portamento_ = 0;
    double portamento_from_ = 0.0;
    double portamento_base_ = 0.0;
    double note_pitch_ = 0.0;
    uint32_t note_start_ = 0;
    int sent_volume_ = 127;
    double sent_pitch_ = 0.0;

    uint32_t notes_ = 0;
    std::optional<ClippedNote> clipped_note_;
    bool sounding_ = false;
    uint32_t off_tick_ = 0;
    std::vector<Action> actions_;
    std::vector<std::pair<uint32_t, int>> tempos_;
};

int ChannelReader::Sized()
{
    const int first = file_.At(place_++);
    if (first < 0x80)
    {
        return first;
    }

    return ((first & 0x7F) << 8) | file_.At(place_++);
}

// Runs one command. Returns false if the channel's list has ended.
bool ChannelReader::RunCommand(uint8_t command)
{
    const auto skip = [&](std::size_t bytes, const char* what)
    {
        place_ += bytes;
        recorder_.Approximate(std::string("the command for ") + what + " left out");
    };

    switch (command)
    {
    case kStop:
        stopped_ = true;
        return false;

    case kWave:
        wave_ = Sized();
        break;

    case kPortamento:
        portamento_ = file_.At(place_++);
        if (portamento_ != 0)
        {
            portamento_base_ = note_pitch_;
        }
        break;

    case kPortamentoOff:
        portamento_ = 0;
        break;

    case kVibratoOff:
        vibrato_speed_ = 0;
        vibrato_ = 0.0;
        break;

    case kWobbleOff:
    case kEnvelopeOn:
    case kTriggerOn:
        break;

    case kEnvelopeBytes:
        SetEnvelope(place_);
        place_ += envelope_.size();
        break;

    case kTempo:
        tempos_.emplace_back(note_end_, TempoOfScript(file_.At(place_++)));
        break;

    case kCutoff:
        cutoff_ = (file_.At(place_) << 8) | file_.At(place_ + 1);
        place_ += 2;
        end_it_ = 0;
        break;

    case kEndIt:
        end_it_ = file_.At(place_++);
        cutoff_ = 0;
        break;

    case kVibratoUp:
    case kVibratoDown:
        vibrato_delay_ = file_.At(place_);
        vibrato_speed_ = file_.At(place_ + 1);
        vibrato_amount_ = (command == kVibratoUp ? 1.0 : -1.0) * file_.At(place_ + 2) / kVibratoUnitsPerSemitone;
        place_ += 3;
        break;

    case kLength:
        fixed_length_ = Sized();
        break;

    case kIgnoreLength:
        ignore_length_ = true;
        break;

    case kTranspose:
        transpose_ = static_cast<int8_t>(file_.At(place_++));
        break;

    case kIgnoreTranspose:
        ignore_transpose_ = true;
        break;

    case kDistort:
        if (file_.At(place_) != 0)
        {
            recorder_.Approximate("a channel's detune left out");
        }

        place_++;
        break;

    case kEnvelope:
        SetEnvelope(file_.envelopes + std::size_t{kEnvelopeSize} * static_cast<std::size_t>(Sized()));
        break;

    case kEnvelopeOff:
        recorder_.Approximate("a note that keeps the last note's envelope");
        break;

    case kTriggerOff:
        recorder_.Approximate("a note that keeps the last note's sample");
        break;

    case kFor:
        {
            if (fors_.size() >= kForDepth)
            {
                throw xsflib::FormatError("a song's loops are too deep");
            }

            const uint8_t count = file_.At(place_++);
            fors_.push_back({count, place_, volume_list_, bend_list_, volume_, bend_, volume_repeat_, bend_repeat_});
            if (count == kForever && !loop_start_)
            {
                loop_start_ = note_end_;
            }
            break;
        }

    case kNext:
        {
            if (fors_.empty())
            {
                throw xsflib::FormatError("a song's Next has no For");
            }

            OpenFor& open = fors_.back();
            if (open.count == kForever)
            {
                if (!loop_end_)
                {
                    loop_end_ = note_end_;
                }

                if (stop_at_loop_)
                {
                    return false;
                }
            }
            else if (--open.count == 0)
            {
                fors_.pop_back();
                break;
            }

            place_ = open.place;
            volume_list_ = open.volume_place;
            bend_list_ = open.bend_place;
            volume_ = open.volume;
            bend_ = open.bend;
            volume_repeat_ = open.volume_repeat;
            bend_repeat_ = open.bend_repeat;

            // The early player sets the bend range again here (the code at 0x80038790 in Pokémon Stadium). The later
            // player keeps the range (the code at 0x80053544 in Gex 64). Each pass of that game's songs ends with a
            // range of 0, and the game then plays no bend until the song sets a range again.
            if (settings_.early_player)
            {
                bend_range_ = kDefaultBendRange;
            }

            bend_semitones_ = bend_ * bend_range_;
            break;
        }

    case kWobble:
        if (file_.At(place_) != 0 && file_.At(place_ + 1) != 0)
        {
            recorder_.Approximate("a pitch wobble left out");
        }

        place_ += 3;
        break;

    case kVelocityOn:
        velocity_on_ = true;
        break;

    case kVelocityOff:
        velocity_on_ = false;
        break;

    case kDefaultVelocity:
        default_velocity_ = file_.At(place_++);
        velocity_on_ = false;
        break;

    case kPan:
        pan_ = file_.At(place_++) / 2;
        break;

    case kStereo:
        place_ += 2;
        break;

    case kDrums:
        drums_ = file_.drums + kDrumSize * static_cast<uint32_t>(Sized());
        break;

    case kDrumsOff:
        drums_.reset();
        break;

    case kPrint:
        place_++;
        break;

    case kGoto:
        {
            const std::size_t to = static_cast<std::size_t>((file_.At(place_) << 8) | file_.At(place_ + 1));
            const std::size_t volume = static_cast<std::size_t>((file_.At(place_ + 2) << 8) | file_.At(place_ + 3));
            const std::size_t bend = static_cast<std::size_t>((file_.At(place_ + 4) << 8) | file_.At(place_ + 5));
            place_ = base_ + to;
            volume_list_ = volume_base_ + volume;
            bend_list_ = bend_base_ + bend;
            volume_repeat_ = 1;
            bend_repeat_ = 1;
            break;
        }

    case kReverb:
        reverb_ = file_.At(place_++);
        break;

    case kRandomNote:
    case kRandomVolume:
    case kRandomPan:
        skip(2, "random values");
        break;

    case kVolume:
        volume_ = file_.At(place_++);
        break;

    case kStartEffect:
        Sized();
        recorder_.Approximate("a song's start of a sound effect left out");
        break;

    case kBendRange:
        bend_range_ = file_.At(place_++) / 64.0;

        // Both players that play songs of the early form square the bend here, up to the bend list's next value: the
        // early player of Pokémon Stadium and the later player of Gex 64 (the code at 0x800538FC).
        bend_semitones_ = file_.early ? bend_ * bend_ : bend_ * bend_range_;
        break;

    case kSweep:
        if (file_.At(place_) != 0)
        {
            recorder_.Approximate("a pan's sweep left out");
        }

        place_++;
        break;

    case kChangeEffect:
        skip(1, "a change of the effect");
        break;

    case kMarker:
        place_++;
        Sized();
        break;

    case kLengthOff:
        fixed_length_ = 0;
        break;

    default:
        throw xsflib::FormatError("a song holds an unknown command");
    }

    return true;
}

void ChannelReader::SetEnvelope(std::size_t at)
{
    for (std::size_t i = 0; i < envelope_.size(); i++)
    {
        envelope_[i] = file_.At(at + i);
    }

    // The early player's notes all start at nothing.
    if (settings_.early_player)
    {
        envelope_[kEnvelopeFirst] = 0;
    }
}

void ChannelReader::EndCurrentNote(uint32_t tick)
{
    if (sounding_)
    {
        Action off;
        off.tick = std::min(off_tick_, tick);
        off.kind = Action::Kind::kOff;
        off.note = notes_ - 1;
        actions_.push_back(off);
        sounding_ = false;
    }
}

void ChannelReader::StartNote(int note)
{
    // A portamento starts from where the last one ended.
    portamento_from_ = portamento_base_;

    int velocity = velocity_;
    if (velocity_on_)
    {
        int value = file_.At(place_++);
        if (value >= 0x80)
        {
            value &= 0x7F;
            velocity_on_ = false;
            default_velocity_ = value;
        }

        if (note != kRest)
        {
            velocity = value;
        }
    }
    else
    {
        velocity = default_velocity_;
    }

    velocity_ = velocity;

    if (fixed_length_ == 0)
    {
        length_ = Sized();
    }
    else if (ignore_length_)
    {
        ignore_length_ = false;
        length_ = Sized();
    }
    else
    {
        length_ = fixed_length_;
    }

    const uint32_t start = note_end_;
    note_end_ += static_cast<uint32_t>(length_);

    if (!drums_ && file_.WaveOf(wave_) < 0)
    {
        note = kRest;
    }

    // A rest lets the last note's release run. A note's end has started that release already.
    EndCurrentNote(start);
    if (note == kRest)
    {
        return;
    }

    if (drums_)
    {
        const std::size_t at = *drums_ + std::size_t{kDrumSize} * static_cast<std::size_t>(note);
        wave_ = (file_.At(at) << 8) | file_.At(at + 1);
        const std::size_t envelope =
            file_.envelopes + std::size_t{kEnvelopeSize} * ((std::size_t{file_.At(at + 2)} << 8) | file_.At(at + 3));
        pan_ = file_.At(at + 4) / 2;
        note = file_.At(at + 5);
        for (std::size_t i = 0; i < envelope_.size(); i++)
        {
            envelope_[i] = file_.At(envelope + i);
        }
    }

    // Two notes in one frame leave the second.
    if (length_ == 0)
    {
        recorder_.Approximate("a note of no length left out");
        return;
    }

    const int bank_wave = file_.WaveOf(wave_);
    if (bank_wave < 0 || static_cast<std::size_t>(bank_wave) >= detunes_.size())
    {
        throw xsflib::FormatError("a song's wave isn't in its bank");
    }

    const int transposed = note + (ignore_transpose_ ? 0 : transpose_);
    const double detune = detunes_[static_cast<std::size_t>(bank_wave)];
    const double semitones = transposed + detune + bend_semitones_;
    note_pitch_ = note + detune;
    note_start_ = start;

    // The player silences a note above an octave, and a voice's volume stops at its top.
    int heard = velocity;
    if (settings_.clipped_envelopes)
    {
        // The note keeps its velocity. ClipStartedNote sets where its preset's envelope stops.
        clipped_note_ = {actions_.size(), velocity};
    }
    else if (const int64_t product = int64_t{volume_} * TopOfEnvelope(envelope_) * settings_.song_volume; product > 0)
    {
        // The velocity takes the stop, at the envelope's top. The early player's volume stops at its top after the
        // master volume. The later player's stops before.
        int64_t limit = kFullVoiceProduct / product;
        if (settings_.early_player)
        {
            limit = limit * (kHighestVoiceVolume + 1) / settings_.master_volume;
        }

        if (heard > limit)
        {
            heard = static_cast<int>(limit);
            recorder_.Approximate("a voice held at the top of its volume, for as long as its envelope is at its top");
        }
    }

    if (semitones > kHighestSemitones)
    {
        heard = 0;
    }

    Action action;
    action.tick = start;
    action.kind = Action::Kind::kNote;
    action.note = notes_++;
    action.key = static_cast<uint8_t>(std::clamp(transposed + kKeyOfNote, 0, 127));

    // A silent note still cuts the last note. The lowest velocity keeps that.
    action.velocity = static_cast<uint8_t>(std::clamp(heard, 1, 127));
    action.pan = static_cast<uint8_t>(std::clamp(pan_, 0, 127));

    // The game's reverb base raises every level the song sets toward full.
    const int base = settings_.reverb_base;
    const int effect = (base + (128 - base) * reverb_ / 128) & 0xFF;
    action.reverb = static_cast<uint8_t>(std::clamp(effect, 0, 127));
    action.preset = {bank_wave, envelope_};
    actions_.push_back(action);

    sounding_ = true;
    if (length_ == kEndlessLength)
    {
        off_tick_ = std::numeric_limits<uint32_t>::max();
    }
    else if (cutoff_ != 0)
    {
        off_tick_ = start + static_cast<uint32_t>(cutoff_);
    }
    else
    {
        // A MIDI note sounds only when it lasts a tick or more. A note whose release starts at once gets one tick.
        off_tick_ = std::max(note_end_ - std::min(static_cast<uint32_t>(end_it_), note_end_), start + 1);
    }
}

void ChannelReader::Step()
{
    // The commands up to the next note, and the note.
    for (int i = 0; i < kMostCommandsInAStep; i++)
    {
        const uint8_t byte = file_.At(place_++);
        if (byte < 0x80)
        {
            StartNote(byte);

            return;
        }

        if (!RunCommand(byte))
        {
            return;
        }
    }

    throw xsflib::FormatError("a song's channel loops without a note");
}

void ChannelReader::ReadVolume()
{
    if (volume_base_ == 0 || --volume_repeat_ != 0)
    {
        return;
    }

    int value = file_.At(volume_list_++);
    volume_repeat_ = 1;
    if (value > 127)
    {
        value &= 0x7F;
        const int count = file_.At(volume_list_++);
        volume_repeat_ = count > 127 ? ((count & 0x7F) * 256) + file_.At(volume_list_++) + 2 : count + 2;
    }

    volume_ = value;
}

// The voice volume holds at a stop value while the envelope is above that value. In each frame, the game starts the
// notes, then reads the volume list of each channel, then sets the voice volume (the code at 0x800545AC, 0x800545F4 and
// 0x80054708 in Gex 64). The stop therefore uses the channel volume as read in this tick. The envelope of the preset
// stops at that value.
void ChannelReader::ClipStartedNote()
{
    if (!clipped_note_)
    {
        return;
    }

    LibmusPresetKey& preset = actions_[clipped_note_->action].preset;
    const int64_t top = int64_t{TopOfEnvelope(preset.envelope)} * kTopSteps;
    const int64_t each = int64_t{volume_} * clipped_note_->velocity * settings_.song_volume;
    const int64_t stop = each > 0 ? kFullVoiceProduct * kTopSteps / each : top;
    if (stop < top)
    {
        preset.top = static_cast<uint16_t>(stop);
    }

    clipped_note_.reset();
}

void ChannelReader::ReadBend()
{
    if (bend_base_ == 0 || --bend_repeat_ != 0)
    {
        return;
    }

    int value = file_.At(bend_list_++);
    bend_repeat_ = 1;
    if (value > 127)
    {
        value &= 0x7F;
        const int count = file_.At(bend_list_++);
        bend_repeat_ = count > 127 ? ((count & 0x7F) * 256) + file_.At(bend_list_++) + 2 : count + 2;
    }

    bend_ = value - 64.0;
    bend_semitones_ = bend_ * bend_range_;
}

// The pitch that a note's vibrato and portamento give it at a tick, in semitones from the note.
double ChannelReader::PitchOfNoteAt(uint32_t tick)
{
    const uint32_t ticks = tick - note_start_;

    double pitch = 0.0;
    if (portamento_ != 0)
    {
        double at = note_pitch_;
        if (ticks <= static_cast<uint32_t>(portamento_))
        {
            at = portamento_from_ + ((note_pitch_ - portamento_from_) * ticks / portamento_);
        }

        portamento_base_ = at;
        pitch += at - note_pitch_;
    }

    if (vibrato_speed_ != 0)
    {
        if (settings_.early_player)
        {
            // The early player's count wraps before the delay's end, to near 2^32. A float keeps the argument that the
            // game's sinf takes. For a speed up to 100, the argument is 2^28 or more, and the game's sinf returns 0.
            // std::sin returns another value there.
            const uint32_t count = ticks - static_cast<uint32_t>(vibrato_delay_);
            if (count != 0)
            {
                const float turns = static_cast<float>(count) / static_cast<float>(vibrato_speed_) * 2.0F;
                vibrato_ = std::sin(static_cast<double>(static_cast<float>(static_cast<double>(turns) * kPi))) *
                           vibrato_amount_;
            }
        }
        else
        {
            // The later player computes the vibrato after its delay. Gex 64's code keeps the last value up to there
            // (0x800551BC), and only the command that ends a vibrato sets it to 0. Songs of the later form take 0 up
            // to there. Their games' code hasn't been read for this.
            const int count = static_cast<int>(ticks) - vibrato_delay_;
            if (count > 0)
            {
                vibrato_ = std::sin(static_cast<double>(count) * 2.0 * kPi / vibrato_speed_) * vibrato_amount_;
            }
            else if (!file_.early)
            {
                vibrato_ = 0.0;
            }
        }

        pitch += vibrato_;
    }

    return pitch;
}

// Sends what a tick's reads changed. A voice takes no new pitch once its release has started.
void ChannelReader::SendChanges(uint32_t tick)
{
    if (volume_ != sent_volume_)
    {
        sent_volume_ = volume_;
        Action action;
        action.tick = tick;
        action.kind = Action::Kind::kVolume;
        action.velocity = static_cast<uint8_t>(volume_);
        actions_.push_back(action);
    }

    if (!sounding_ || tick >= off_tick_)
    {
        return;
    }

    const double pitch = bend_semitones_ + PitchOfNoteAt(tick);
    if (pitch != sent_pitch_)
    {
        sent_pitch_ = pitch;
        Action action;
        action.tick = tick;
        action.kind = Action::Kind::kBend;
        action.semitones = static_cast<float>(pitch);
        actions_.push_back(action);
    }
}

void ChannelReader::Run(uint32_t end)
{
    for (uint32_t tick = 0; tick < end; tick++)
    {
        while (!stopped_ && note_end_ <= tick && length_ != kEndlessLength)
        {
            Step();
        }

        if (stopped_)
        {
            ClipStartedNote();
            EndCurrentNote(tick);
            return;
        }

        ReadVolume();
        ClipStartedNote();
        ReadBend();
        SendChanges(tick);
    }

    EndCurrentNote(end);
}

void ChannelReader::RunToLoop()
{
    stop_at_loop_ = true;
    while (!stopped_ && !loop_end_ && length_ != kEndlessLength)
    {
        Step();
    }
}

// Scales a song's tempos the way the game's code does a few frames after it starts the song. The library multiplies in
// whole numbers.
void ScaleTempos(std::vector<std::pair<uint32_t, int>>& tempos, const LibmusSettings& settings)
{
    for (auto& [tick, tempo] : tempos)
    {
        tempo = tempo * settings.tempo_scale / kFullTempoScale;
    }
}

// What a pass of a channel plays, from tick `from` for `length` ticks: its notes, their ends, and the pitch and volume
// at each note's start and wherever they change under a note of the pass. Ticks count from the pass's start, and a
// note's number from its first note. The main loop's end cuts every note. The end of a note from before the pass
// therefore doesn't count. Nor does a pitch or volume that no note of the pass plays.
std::vector<Action> HeardInPass(std::vector<Action> actions, uint32_t from, uint32_t length)
{
    // A note's end goes in when the next note starts, after the settings of the ticks between.
    std::ranges::stable_sort(actions, {}, &Action::tick);

    double pitch = 0.0;
    int volume = 127;
    uint32_t notes_before = 0;
    std::size_t i = 0;
    for (; i < actions.size() && actions[i].tick < from; i++)
    {
        const Action& action = actions[i];
        notes_before += action.kind == Action::Kind::kNote ? 1 : 0;
        pitch = action.kind == Action::Kind::kBend ? action.semitones : pitch;
        volume = action.kind == Action::Kind::kVolume ? action.velocity : volume;
    }

    std::vector<Action> heard;
    int sounding = 0;
    std::optional<std::pair<double, int>> last_heard;
    while (i < actions.size() && actions[i].tick - from < length)
    {
        // The actions of one tick. A note plays the pitch and the volume that its tick sets.
        const uint32_t tick = actions[i].tick;
        bool started = false;
        for (; i < actions.size() && actions[i].tick == tick; i++)
        {
            Action action = actions[i];
            action.tick -= from;
            action.note -= notes_before;
            switch (action.kind)
            {
            case Action::Kind::kOff:
                if (actions[i].note >= notes_before)
                {
                    heard.push_back(action);
                    sounding--;
                }
                break;

            case Action::Kind::kVolume:
                volume = action.velocity;
                break;

            case Action::Kind::kBend:
                pitch = action.semitones;
                break;

            case Action::Kind::kNote:
                heard.push_back(action);
                sounding++;
                started = true;
                break;
            }
        }

        if (sounding == 0)
        {
            last_heard.reset();
            continue;
        }

        if (started || last_heard != std::pair{pitch, volume})
        {
            Action level;
            level.tick = tick - from;
            level.kind = Action::Kind::kBend;
            level.semitones = static_cast<float>(pitch);
            level.velocity = static_cast<uint8_t>(volume);
            heard.push_back(level);
            last_heard = {pitch, volume};
        }
    }

    return heard;
}

} // namespace

LibmusPerformance PerformLibmus(std::span<const uint8_t> song, std::span<const double> detunes,
                                const LibmusSettings& settings, const PerformOptions& options)
{
    const SongFile file = SongFile::Parse(song, settings.early_form);

    midi::Recorder recorder;
    LibmusPerformance out;
    Performance& performance = out.performance;
    performance.timebase = kTicksPerQuarter;

    const auto reader_of = [&](uint32_t channel, midi::Recorder& to)
    {
        const bool master = channel == file.channels;
        return ChannelReader(file, detunes, settings, master ? file.master_track : file.Entry(file.data_list, channel),
                             master ? 0 : file.Entry(file.volume_list, channel),
                             master ? 0 : file.Entry(file.bend_list, channel), to);
    };

    // A new pass keeps most of a channel's settings from the end of the last pass. A channel's second pass can
    // therefore differ from its first. Gex 64's bends do. Every later pass repeats the second. Returns whether the
    // second pass plays as the first, or nothing if the second pass can't be read.
    midi::Recorder walk_recorder;
    const auto second_pass_repeats = [&](uint32_t channel, uint32_t start, uint32_t length) -> std::optional<bool>
    {
        try
        {
            ChannelReader reader = reader_of(channel, walk_recorder);
            reader.Run(start + 2 * length);

            return HeardInPass(reader.Actions(), start, length) ==
                   HeardInPass(reader.Actions(), start + length, length);
        }
        catch (const xsflib::FormatError&)
        {
            return std::nullopt;
        }
    };

    // A first walk finds the main loop: where every channel is inside its endless loop, and the shortest length that
    // brings them all back to where they were.
    const auto listed = [&](uint32_t channel)
    {
        return channel == file.channels ? file.master_track != 0 : file.Entry(file.data_list, channel) != 0;
    };

    uint32_t loop_start = 0;
    uint32_t later_loop_start = 0;
    bool later_passes_read = settings.loop_on_later_passes;
    uint64_t loop_length = 0;
    bool loops = true;
    uint32_t last_end = 0;
    for (uint32_t channel = 0; channel <= file.channels; channel++)
    {
        if (!listed(channel))
        {
            continue;
        }

        const bool master = channel == file.channels;
        ChannelReader reader(file, detunes, settings, master ? file.master_track : file.Entry(file.data_list, channel),
                             0, 0, walk_recorder);
        reader.RunToLoop();
        last_end = std::max(last_end, reader.EndTick());
        if (!reader.LoopStart() || !reader.LoopEnd() || *reader.LoopEnd() <= *reader.LoopStart())
        {
            loops = false;
            continue;
        }

        const uint32_t start = *reader.LoopStart();
        const uint32_t length = *reader.LoopEnd() - start;
        loop_length = loop_length == 0 ? length : std::lcm(loop_length, uint64_t{length});

        loop_start = std::max(loop_start, start);
        if (later_passes_read)
        {
            // Where a channel's second pass differs from its first, the main loop starts at the second.
            const std::optional<bool> repeats = second_pass_repeats(channel, start, length);
            later_passes_read = repeats.has_value();
            later_loop_start = std::max(later_loop_start, repeats.value_or(true) ? start : start + length);
        }
    }

    // If any channel's second pass can't be read, every channel's loop stays at its first pass.
    if (later_passes_read)
    {
        loop_start = later_loop_start;
    }

    // Each channel's actions, and the tempo that any of them sets.
    uint32_t end_tick =
        loops ? static_cast<uint32_t>(std::min<uint64_t>(loop_start + loop_length, kLongestLoopEnd)) : last_end;
    std::vector<std::vector<Action>> channels;
    std::vector<std::pair<uint32_t, int>> tempos;
    for (uint32_t channel = 0; channel <= file.channels; channel++)
    {
        if (!listed(channel))
        {
            channels.emplace_back();
            continue;
        }

        ChannelReader reader = reader_of(channel, recorder);
        reader.Run(end_tick);
        tempos.insert(tempos.end(), reader.Tempos().begin(), reader.Tempos().end());
        channels.push_back(reader.Actions());
    }

    // The tempo.
    std::ranges::stable_sort(tempos, {}, [](const auto& tempo) { return tempo.first; });
    if (tempos.empty() || tempos[0].first != 0)
    {
        tempos.insert(tempos.begin(), {0, kDefaultTempo});
    }

    tempos.erase(std::ranges::unique(tempos, {}, [](const auto& tempo) { return tempo.first; }).begin(), tempos.end());
    ScaleTempos(tempos, settings);
    const auto seconds_at = [&](uint32_t tick)
    {
        double seconds = 0.0;
        for (std::size_t i = 0; i < tempos.size(); i++)
        {
            const uint32_t from = tempos[i].first;
            if (from >= tick)
            {
                break;
            }

            const uint32_t to = i + 1 < tempos.size() ? std::min(tempos[i + 1].first, tick) : tick;
            seconds += (to - from) / TicksPerSecond(tempos[i].second, settings.output_rate);
        }

        return seconds;
    };

    if (loops && seconds_at(end_tick) > kLongestSeconds)
    {
        recorder.Approximate("a song cut at " + std::to_string(static_cast<int>(kLongestSeconds)) +
                             " seconds, before its channels' loops come back together");
        uint32_t tick = 0;
        while (seconds_at(tick) < kLongestSeconds)
        {
            tick += kTicksPerQuarter;
        }

        end_tick = tick;
        loops = false;
    }

    for (const auto& [tick, tempo] : tempos)
    {
        recorder.SetTempo(performance, tick, TicksPerSecond(tempo, settings.output_rate) * 60.0 / kTicksPerQuarter);
    }

    if (tempos.size() > 1)
    {
        recorder.Approximate("a song's change of tempo, which stays out of its envelopes' times");
    }

    out.ticks_per_second = TicksPerSecond(tempos[0].second, settings.output_rate);

    // Channels with notes get a MIDI track. More channels than tracks share: the two whose notes meet least.
    std::vector<std::vector<std::size_t>> tracks;
    for (std::size_t channel = 0; channel < channels.size(); channel++)
    {
        if (std::ranges::any_of(channels[channel], [](const Action& a) { return a.kind == Action::Kind::kNote; }))
        {
            tracks.push_back({channel});
        }
    }

    const auto spans_of = [&](const std::vector<std::size_t>& group)
    {
        std::vector<std::pair<uint32_t, uint32_t>> spans;
        for (const std::size_t channel : group)
        {
            const std::vector<Action>& actions = channels[channel];
            for (std::size_t i = 0; i < actions.size(); i++)
            {
                if (actions[i].kind == Action::Kind::kNote)
                {
                    uint32_t until = end_tick;
                    for (std::size_t j = i + 1; j < actions.size(); j++)
                    {
                        if (actions[j].kind == Action::Kind::kNote)
                        {
                            until = actions[j].tick;
                            break;
                        }
                    }

                    spans.emplace_back(actions[i].tick, until);
                }
            }
        }

        return spans;
    };

    while (tracks.size() > kMidiTracks)
    {
        std::size_t best_a = 0;
        std::size_t best_b = 1;
        uint64_t best = std::numeric_limits<uint64_t>::max();
        for (std::size_t a = 0; a < tracks.size(); a++)
        {
            const auto spans_a = spans_of(tracks[a]);
            for (std::size_t b = a + 1; b < tracks.size(); b++)
            {
                uint64_t overlap = 0;
                for (const auto& [from_b, to_b] : spans_of(tracks[b]))
                {
                    for (const auto& [from_a, to_a] : spans_a)
                    {
                        const uint32_t from = std::max(from_a, from_b);
                        const uint32_t to = std::min(to_a, to_b);
                        overlap += to > from ? to - from : 0;
                    }
                }

                if (overlap < best)
                {
                    best = overlap;
                    best_a = a;
                    best_b = b;
                }
            }
        }

        tracks[best_a].insert(tracks[best_a].end(), tracks[best_b].begin(), tracks[best_b].end());
        tracks.erase(tracks.begin() + static_cast<std::ptrdiff_t>(best_b));
        recorder.Approximate("more channels than a MIDI file holds: two share a track, and their notes meet for " +
                             std::to_string(best) + " ticks");
    }

    // The actions in time order: a note's end first, then a setting, then the next note.
    for (std::size_t index = 0; index < tracks.size(); index++)
    {
        midi::TrackOutput output;
        output.midi_track = static_cast<uint8_t>(index);
        recorder.SendSetting(performance, output, 0, midi::EventKind::kVolume, 127);
        recorder.SendSetting(performance, output, 0, midi::EventKind::kExpression, 127);
        recorder.SendSetting(performance, output, 0, midi::EventKind::kReverb, 0);
        recorder.SendSetting(performance, output, 0, midi::EventKind::kPan, 64);

        struct Placed
        {
            Action action;
            std::size_t channel = 0;
        };

        std::vector<Placed> placed;
        for (const std::size_t channel : tracks[index])
        {
            for (const Action& action : channels[channel])
            {
                placed.push_back({action, channel});
            }
        }

        std::ranges::stable_sort(placed,
                                 [](const Placed& a, const Placed& b)
                                 {
                                     if (a.action.tick != b.action.tick)
                                     {
                                         return a.action.tick < b.action.tick;
                                     }

                                     return a.action.kind < b.action.kind;
                                 });

        std::map<std::pair<std::size_t, uint32_t>, std::pair<uint8_t, uint32_t>> started;
        for (const Placed& entry : placed)
        {
            const Action& action = entry.action;
            switch (action.kind)
            {
            case Action::Kind::kVolume:
                recorder.SendSetting(performance, output, action.tick, midi::EventKind::kVolume, action.velocity);
                break;

            case Action::Kind::kBend:
                recorder.SendPitch(performance, output, action.tick, action.semitones);
                break;

            case Action::Kind::kNote:
                {
                    recorder.SendPreset(performance, output, action.tick,
                                        midi::PresetIndex(out.presets, action.preset));
                    recorder.SendSetting(performance, output, action.tick, midi::EventKind::kPan, action.pan);
                    recorder.SendSetting(performance, output, action.tick, midi::EventKind::kReverb, action.reverb);

                    // The mixer takes the send's share from the dry level, on the sin/cos law. The controller acts
                    // squared.
                    recorder.SendSetting(performance, output, action.tick, midi::EventKind::kExpression,
                                         static_cast<uint8_t>(std::lround(127.0 * std::sqrt(DryShare(action.reverb)))));
                    const uint32_t id = recorder.NoteOn(performance, output, action.tick, action.key, action.velocity);
                    started[{entry.channel, action.note}] = {action.key, id};
                    break;
                }

            case Action::Kind::kOff:
                {
                    const auto& [key, id] = started.at({entry.channel, action.note});
                    recorder.NoteOff(performance, output.midi_track, action.tick, 0, key, id);
                    break;
                }
            }
        }
    }

    if (loops)
    {
        midi::EndPassAt(performance, end_tick);
        performance.loop = midi::Performance::Loop{loop_start, end_tick};
        performance.loops = options.loops;
    }

    performance.end_tick = end_tick;
    performance.seconds = seconds_at(end_tick);
    performance.approximations = recorder.Approximations();
    performance.warnings = recorder.Warnings();

    return out;
}

} // namespace ultralib::sequence_export::n64
