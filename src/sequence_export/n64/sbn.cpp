// SPDX-License-Identifier: MIT

#include "ultralib/sequence_export/n64/sbn.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "seqlib/midi/recorder.h"
#include "xsflib/bytes.h"

#include "rsp_audio.h"
#include "sbn_shared.h"

namespace ultralib::sequence_export::n64
{

namespace
{

namespace n64 = ultralib::n64;
namespace midi = seqlib::midi;
using sbn::DrumOf;
using sbn::kCentrePan;
using sbn::kCentsPerKey;
using sbn::kFullLevel;
using sbn::kHighestPan;
using sbn::kHighestRatio;
using sbn::kLevelMask;
using sbn::kNominalRate;
using sbn::kVelocityScale;

// The clock.
constexpr uint32_t kFrameSamples = 184;
constexpr uint32_t kMilliFramesPerMinute = 1000 * 60 * kNominalRate / kFrameSamples;
constexpr uint32_t kShortestInterval = 80000;
constexpr uint32_t kLongestInterval = 500000;
constexpr uint32_t kStepsPerBeatsPerMinute = 1000;
constexpr int kDefaultBeatsPerMinute = 156;

// The voices.
constexpr std::size_t kVoiceCount = n64::kBgmTrackCount * 4 + 1;
constexpr std::array<int, 8> kVoicesOfPolyphony = {0, 1, 0, 0, 0, 2, 3, 4};
constexpr int kFirstSharedPolyphony = 5;
constexpr std::size_t kSongInstruments = 128;

// The levels. The master volume and an instrument's volume are numbers with 24 bits below the point.
constexpr int kFractionBits = 24;
constexpr int64_t kFullVolume = int64_t{kFullLevel} << kFractionBits;
constexpr int64_t kFirstInstrumentVolume = int64_t{0x7FFF} << 16;

// The game's code waits a few frames after it starts a song before it sets the levels of the song's tracks. A song's
// first commands would undo them. Each level then takes this many ticks to reach.
constexpr double kTrackLevelsSeconds = 0.2;
constexpr int kTrackLevelsFadeTicks = 48;

// The commands of a track's special command.
constexpr uint8_t kSpecialStereoDelay = 1;
constexpr uint8_t kSpecialSeekCustom = 2;
constexpr uint8_t kSpecialWriteCustom = 3;
constexpr uint8_t kSpecialUseCustom = 4;
constexpr uint8_t kSpecialSound = 5;
constexpr uint8_t kSpecialProximity = 6;

constexpr std::size_t kCustomEnvelopes = 8;
constexpr std::size_t kCustomSteps = 9;
constexpr uint16_t kCustomEmpty = 0xFF00;

constexpr int kLoopDepth = 4;
constexpr int kLoopLabels = 32;
constexpr int kMostPassesInATick = 100;
constexpr double kNoEnd = std::numeric_limits<double>::infinity();

// A tremolo's phase goes round in 256 steps, and its peak comes just short of its depth.
constexpr double kTremoloPeriod = 256.0;
constexpr double kTremoloPeak = 255.0 / 256.0;

int WordAt(const n64::BgmCommand& command, std::size_t at)
{
    return command.args[at] << 8 | command.args[at + 1];
}

uint8_t VelocityOf(int velocity)
{
    return static_cast<uint8_t>(std::clamp<long>(std::lround((velocity + 1) * kFullLevel / kVelocityScale), 1, 127));
}

class Performer
{
public:
    Performer(const n64::BgmFile& file, std::span<const n64::BgmDrum> per, std::span<const n64::BgmInstrument> prg,
              const SbnFacts& facts, const SbnSettings& settings, const PerformOptions& options);

    SbnPerformance Run();

private:
    struct Track
    {
        bool active = false;
        n64::BgmCursor cursor;
        int delay = 0;
        bool drums = false;
        bool muted = false;
        bool started = false; // a note has played
        int polyphony = 0;
        int first_voice = 0;
        int last_voice = 0;
        std::optional<uint16_t> bank_patch;
        int64_t instrument_volume = kFirstInstrumentVolume;
        int fade_ticks = 0;
        int64_t fade_step = 0;
        int64_t fade_target = 0;
        int pan = kCentrePan;
        int random_pan = 0;
        int reverb = 0;
        int volume = kFullLevel;
        int proximity = kFullLevel;
        bool proximity_changed = false; // the game's code has changed the mix, and no branch has read it yet
        int proximity_full = 0;
        int proximity_near = 0;
        int coarse = 0;
        int fine = 0;
        int detune = 0;
        int tremolo_delay = 0;
        int tremolo_rate = 0;
        int tremolo_depth = 0;
        int custom_press = 0;
        midi::TrackOutput output;
    };

    struct Voice
    {
        bool busy = false;
        bool pending = false; // it started in this tick
        int length = 0;
        double free_at = kNoEnd;
        bool sounding = false;
        double release_seconds = 0.0;
        uint32_t note = 0;
        uint8_t midi_track = 0;
        uint8_t key = 0;
    };

    void ReadComposition();
    void EndCompositionLoop(const n64::BgmCompositionCommand& command);
    void LoadPhrase(uint32_t offset);
    bool RunTracks();
    void RunCommand(Track& track, const n64::BgmCommand& command);
    void RunSpecial(Track& track, const n64::BgmCommand& command);
    void FadeInstrument(Track& track, int time, int level);
    void ApplyTrackLevels();
    void UseInstrument(Track& track, int index);
    void StartNote(Track& track, const n64::BgmCommand& command, int& scan);
    void StartInstrumentNote(Track& track, const n64::BgmCommand& command, Voice& voice);
    void StartDrumNote(Track& track, const n64::BgmCommand& command, Voice& voice);
    std::optional<std::size_t> AcquireVoice(Track& track, int& scan);
    void RunVoices(Track& track);
    void CheckDone(Voice& voice);
    void ReleaseVoice(Voice& voice);
    void ResetVoice(Voice& voice);
    void SendLevels(Track& track);
    void SendAllLevels();
    void SetTicksPerBeat(int ticks_per_beat);
    void SetTempo(int64_t tempo);
    void AdvanceClock();
    double TicksPerSecond() const;
    int TremoloCents(const Track& track) const;

    const n64::BgmFile& file_;
    std::span<const n64::BgmDrum> per_;
    std::span<const n64::BgmInstrument> prg_;
    const SbnFacts& facts_;
    const SbnSettings& settings_;
    const PerformOptions& options_;

    midi::Recorder recorder_;
    SbnPerformance out_;
    std::array<Track, n64::kBgmTrackCount> tracks_;
    std::array<Voice, kVoiceCount> voices_;
    std::array<std::array<uint16_t, kCustomSteps>, kCustomEnvelopes> custom_ = {};
    std::array<uint8_t, kCustomEnvelopes> custom_write_ = {};
    int custom_writing_ = 0;

    // The composition.
    uint32_t composition_offset_ = 0;
    std::vector<n64::BgmCompositionCommand> commands_;
    std::size_t position_ = 0;
    std::array<std::size_t, kLoopLabels> labels_ = {};
    std::array<uint32_t, kLoopLabels> label_ticks_ = {};
    std::array<std::optional<std::size_t>, kLoopDepth + 1> loop_ends_ = {};
    std::array<int, kLoopDepth + 1> loop_counts_ = {};
    int loop_depth_ = 0;
    bool link_mutes_left_ = true;
    bool ended_ = false;
    std::optional<std::pair<uint32_t, uint32_t>> loop_;

    // The master settings.
    int64_t master_volume_ = kFullVolume;
    bool track_levels_applied_ = false;
    int master_fade_ticks_ = 0;
    int64_t master_fade_step_ = 0;
    int64_t master_fade_target_ = 0;
    int master_shift_ = 0;
    int64_t tempo_ = int64_t{kDefaultBeatsPerMinute} * 100;
    int tempo_fade_ticks_ = 0;
    int64_t tempo_fade_step_ = 0;
    int64_t tempo_fade_target_ = 0;

    // The clock.
    uint32_t interval_ = 0;
    uint32_t step_ = 0;
    int64_t counter_ = 0;
    uint64_t frames_ = 0;
    uint32_t tick_ = 0;
    double seconds_ = 0.0;
    uint32_t random_ = 0;
};

Performer::Performer(const n64::BgmFile& file, std::span<const n64::BgmDrum> per,
                     std::span<const n64::BgmInstrument> prg, const SbnFacts& facts, const SbnSettings& settings,
                     const PerformOptions& options)
    : file_(file), per_(per), prg_(prg), facts_(facts), settings_(settings), options_(options)
{
    for (auto& steps : custom_)
    {
        steps.fill(kCustomEmpty);
    }

    for (std::size_t i = 0; i < tracks_.size(); i++)
    {
        tracks_[i].output.midi_track = static_cast<uint8_t>(i);
        tracks_[i].proximity_changed = settings.mix != 0;
    }

    SetTicksPerBeat(settings.ticks_per_beat);
}

void Performer::SetTicksPerBeat(int ticks_per_beat)
{
    interval_ =
        std::clamp(kMilliFramesPerMinute / static_cast<uint32_t>(ticks_per_beat), kShortestInterval, kLongestInterval);
    counter_ = interval_;
    step_ = static_cast<uint32_t>(tempo_ * kStepsPerBeatsPerMinute / 100);
}

// A tempo holds hundredths of a beat a minute.
void Performer::SetTempo(int64_t tempo)
{
    tempo_ = tempo;
    step_ = static_cast<uint32_t>(tempo * kStepsPerBeatsPerMinute / 100);
    recorder_.SetTempo(out_.performance, tick_, TicksPerSecond() * 60.0 / settings_.ticks_per_beat);
}

double Performer::TicksPerSecond() const
{
    const double frames_per_second = static_cast<double>(settings_.output_rate) / kFrameSamples;

    return std::min(1.0, static_cast<double>(step_) / interval_) * frames_per_second;
}

// Runs frames up to the next tick.
void Performer::AdvanceClock()
{
    do
    {
        frames_++;
        counter_ -= step_;
    } while (counter_ > 0);

    counter_ += interval_;
    seconds_ = static_cast<double>(frames_) * kFrameSamples / settings_.output_rate;

    // The game's random numbers mix a count of the song's frames and a count of the video's. The conversion takes the
    // video's count from the seed and the frames.
    constexpr uint64_t kFramesPerVideoFrame = 3;
    const uint32_t video = options_.seed + static_cast<uint32_t>(frames_ / kFramesPerVideoFrame);
    random_ = (random_ & 0xFFFF) + static_cast<uint32_t>(frames_ & 0xFFFF) + (video & 0xFFFF);
}

void Performer::SendLevels(Track& track)
{
    Performance& performance = out_.performance;
    recorder_.SendSetting(performance, track.output, tick_, midi::EventKind::kVolume,
                          static_cast<uint8_t>(track.volume));

    // The instrument's volume, the master volume and the send's share of a track's level. The controller acts squared.
    const double instrument = static_cast<double>(track.instrument_volume) / kFullVolume;
    const double master = static_cast<double>(master_volume_) / kFullVolume;
    const double dry = track.drums ? 1.0 : std::sqrt(std::max(DryShare(track.reverb), 0.0));
    const double proximity = static_cast<double>(track.proximity) / kFullLevel;
    const long expression = std::lround(kFullLevel * std::min(instrument, 1.0) * master * dry * proximity);
    recorder_.SendSetting(performance, track.output, tick_, midi::EventKind::kExpression,
                          static_cast<uint8_t>(std::clamp<long>(expression, 0, kFullLevel)));
}

void Performer::SendAllLevels()
{
    for (Track& track : tracks_)
    {
        if (track.active && track.started)
        {
            SendLevels(track);
        }
    }
}

void Performer::EndCompositionLoop(const n64::BgmCompositionCommand& command)
{
    const std::size_t label = command.Label();
    const int count = command.LoopCount();
    if (count == 0)
    {
        // The song loops for ever from here. A loop of no length ends the song.
        ended_ = true;
        if (label_ticks_.at(label) < tick_)
        {
            loop_ = {label_ticks_.at(label), tick_};
        }

        return;
    }

    std::optional<std::size_t>& loop_end = loop_ends_.at(static_cast<std::size_t>(loop_depth_));
    int& loop_count = loop_counts_.at(static_cast<std::size_t>(loop_depth_));
    if (!loop_end)
    {
        loop_end = position_;
        loop_count = count;
        position_ = labels_.at(label);
    }
    else if (*loop_end == position_)
    {
        loop_count--;
        if (loop_count != 0)
        {
            position_ = labels_.at(label);
        }
        else
        {
            loop_end.reset();
            loop_depth_ = std::max(loop_depth_ - 1, 0);
        }
    }
    else if (loop_depth_ < kLoopDepth)
    {
        loop_depth_++;
        loop_ends_.at(static_cast<std::size_t>(loop_depth_)) = position_;
        loop_counts_.at(static_cast<std::size_t>(loop_depth_)) = count;
        position_ = labels_.at(label);
    }
}

void Performer::LoadPhrase(uint32_t offset)
{
    const std::array<n64::BgmTrackInfo, n64::kBgmTrackCount> infos = file_.Phrase(offset);
    int next_voice = 0;
    bool found_link = false;
    for (std::size_t i = 0; i < tracks_.size(); i++)
    {
        const n64::BgmTrackInfo& info = infos[i];
        Track& track = tracks_[i];
        track.active = false;
        if (info.offset == 0 || info.disabled)
        {
            continue;
        }

        track.polyphony = info.polyphony;
        track.drums = info.drums;
        if (info.linked_track != 0)
        {
            const std::size_t linked = info.linked_track - std::size_t{1};
            if (linked >= i)
            {
                continue;
            }

            track.first_voice = tracks_[linked].first_voice;
            track.last_voice = tracks_[linked].last_voice;
            if (link_mutes_left_)
            {
                track.muted = !settings_.linked;
                tracks_[linked].muted = settings_.linked;
            }

            found_link = true;
        }
        else
        {
            track.first_voice = next_voice;
            next_voice += kVoicesOfPolyphony.at(track.polyphony);
            track.last_voice = next_voice;
        }

        track.active = true;
        track.delay = 1;
        track.cursor = {};
        track.cursor.position = info.offset;
    }

    if (found_link)
    {
        link_mutes_left_ = false;
    }
}

void Performer::ReadComposition()
{
    // A new phrase ends a tempo's fade.
    tempo_fade_ticks_ = 0;
    while (!ended_)
    {
        if (position_ >= commands_.size())
        {
            ended_ = true;
            return;
        }

        const n64::BgmCompositionCommand& command = commands_[position_];
        position_++;
        switch (command.Op())
        {
        case n64::BgmCompositionOp::kEnd:
            ended_ = true;
            return;

        case n64::BgmCompositionOp::kPlayPhrase:
            LoadPhrase(command.PhraseOffset(composition_offset_));
            return;

        case n64::BgmCompositionOp::kStartLoop:
            label_ticks_.at(command.Label()) = tick_;
            break;

        case n64::BgmCompositionOp::kEndLoop:
        case n64::BgmCompositionOp::kEndLoopUnless:
            // The game's flag for a conditional loop stays off.
            EndCompositionLoop(command);
            break;

        default:
            break;
        }
    }
}

void Performer::CheckDone(Voice& voice)
{
    if (voice.busy && seconds_ >= voice.free_at)
    {
        ResetVoice(voice);
    }
}

void Performer::ReleaseVoice(Voice& voice)
{
    if (voice.sounding)
    {
        recorder_.NoteOff(out_.performance, voice.midi_track, tick_, 0, voice.key, voice.note);
        voice.sounding = false;
    }

    voice.free_at = std::min(voice.free_at, seconds_ + voice.release_seconds);
}

void Performer::ResetVoice(Voice& voice)
{
    if (voice.sounding)
    {
        recorder_.NoteOff(out_.performance, voice.midi_track, tick_, 0, voice.key, voice.note);
        voice.sounding = false;
    }

    voice.busy = false;
    voice.pending = false;
}

std::optional<std::size_t> Performer::AcquireVoice(Track& track, int& scan)
{
    for (int index = scan; index < track.last_voice; index++)
    {
        scan++;
        Voice& voice = voices_.at(static_cast<std::size_t>(index));
        CheckDone(voice);
        if (!voice.busy)
        {
            return static_cast<std::size_t>(index);
        }
    }

    const std::size_t first = static_cast<std::size_t>(track.first_voice);
    const std::size_t last = static_cast<std::size_t>(track.last_voice);
    if (track.polyphony < kFirstSharedPolyphony)
    {
        ResetVoice(voices_.at(first));

        return first;
    }

    for (std::size_t index = first; index < last; index++)
    {
        if (voices_[index].busy && voices_[index].length == 0)
        {
            ResetVoice(voices_[index]);

            return index;
        }
    }

    std::optional<std::size_t> shortest;
    int least = std::numeric_limits<uint16_t>::max();
    for (std::size_t index = first; index < last; index++)
    {
        if (voices_[index].busy && !voices_[index].pending && voices_[index].length < least)
        {
            least = voices_[index].length;
            shortest = index;
        }
    }

    if (shortest)
    {
        ResetVoice(voices_[*shortest]);
    }

    return shortest;
}

int Performer::TremoloCents(const Track& track) const
{
    return static_cast<int>(std::lround(track.tremolo_depth * kTremoloPeak));
}

void Performer::StartInstrumentNote(Track& track, const n64::BgmCommand& command, Voice& voice)
{
    SbnInstrumentFacts facts;
    if (track.bank_patch)
    {
        facts = facts_(*track.bank_patch);
    }

    voice.busy = true;
    voice.pending = true;
    voice.free_at = seconds_;
    if (!facts.exists)
    {
        if (track.bank_patch)
        {
            recorder_.Approximate("a note on an instrument that the game's banks don't hold");
        }
        else
        {
            recorder_.Approximate("a note on a track with no instrument");
        }

        return;
    }

    voice.release_seconds = facts.release_seconds;

    // The key takes the track's whole semitones, and the preset takes its cents.
    const int cents = command.key * kCentsPerKey + track.coarse + master_shift_ + track.fine - facts.key_base;
    if (facts.loops)
    {
        voice.free_at = kNoEnd;
    }
    else if (facts.sample_rate != 0)
    {
        const double ratio =
            std::min(std::pow(2.0, (cents + track.detune) / 1200.0) * facts.sample_rate / kNominalRate, kHighestRatio);
        voice.free_at = seconds_ + facts.samples / (ratio * settings_.output_rate);
    }

    if (command.velocity == 0)
    {
        return;
    }

    SbnPresetKey key;
    key.bank_patch = *track.bank_patch;
    key.fine_cents = track.fine;
    key.cuts = track.polyphony < kFirstSharedPolyphony;
    if (track.custom_press != 0)
    {
        for (const uint16_t step : custom_.at(static_cast<std::size_t>(track.custom_press - 1)))
        {
            xsflib::AppendBe16(key.press, step);
        }
    }

    if (track.tremolo_depth != 0 && track.tremolo_rate != 0)
    {
        constexpr double kHzSteps = 16.0;
        constexpr double kDelayStepMs = 10.0;
        const double ticks = TicksPerSecond();
        key.tremolo_depth = TremoloCents(track);
        key.tremolo_hz_sixteenths =
            static_cast<int>(std::lround(ticks * track.tremolo_rate / kTremoloPeriod * kHzSteps));
        key.tremolo_delay_ms =
            static_cast<int>(std::lround(track.tremolo_delay * 1000.0 / ticks / kDelayStepMs) * kDelayStepMs);
    }

    int pan = track.pan;
    if (track.random_pan != 0)
    {
        // The engine's tap of its random number decides the side, and other bits decide the distance.
        const int value = static_cast<int>(random_ >> 8 & 0x3F) + static_cast<int>(random_ << 4 & 0xC0);
        const int shift = track.random_pan * value >> 8;
        const bool right = (((random_ >> 7) + (random_ >> 2)) & 1) != 0;
        pan = std::clamp(right ? pan + shift : pan - shift, 0, kHighestPan);
    }

    Performance& performance = out_.performance;
    track.started = true;
    SendLevels(track);
    recorder_.SendPreset(performance, track.output, tick_, static_cast<uint32_t>(midi::PresetIndex(out_.presets, key)));
    recorder_.SendSetting(performance, track.output, tick_, midi::EventKind::kPan, static_cast<uint8_t>(pan));
    recorder_.SendSetting(performance, track.output, tick_, midi::EventKind::kReverb,
                          static_cast<uint8_t>(track.reverb));
    recorder_.SendPitch(performance, track.output, tick_, static_cast<float>(track.detune) / kCentsPerKey);
    const int shifted = command.key + (track.coarse + master_shift_) / kCentsPerKey;
    voice.key = static_cast<uint8_t>(std::clamp(shifted, 0, 127));
    voice.midi_track = track.output.midi_track;
    voice.note = recorder_.NoteOn(performance, track.output, tick_, voice.key, VelocityOf(command.velocity));
    voice.sounding = true;
}

void Performer::StartDrumNote(Track& track, const n64::BgmCommand& command, Voice& voice)
{
    const int drum_key = command.key;
    const n64::BgmDrum* drum = DrumOf(file_, per_, drum_key);
    voice.busy = true;
    voice.pending = true;
    voice.free_at = seconds_;
    if (drum == nullptr)
    {
        return;
    }

    const SbnInstrumentFacts facts = facts_(drum->bank_patch);
    if (!facts.exists)
    {
        recorder_.Approximate("a drum on an instrument that the game's banks don't hold");
        return;
    }

    voice.release_seconds = facts.release_seconds;
    int tune = track.detune;
    if (drum->random_tune != 0)
    {
        const int value = static_cast<int>(random_ >> 6 & 0xF) + static_cast<int>(random_ << 2 & 0xF0);
        const int shift = drum->random_tune * 5 * value >> 8;
        tune += (((random_ >> 4) + (random_ >> 1)) & 1) != 0 ? shift : -shift;
    }

    if (facts.loops)
    {
        voice.free_at = kNoEnd;
    }
    else if (facts.sample_rate != 0)
    {
        const int cents = drum->key_base + track.coarse + track.fine - facts.key_base + tune;
        const double ratio = std::min(std::pow(2.0, cents / 1200.0) * facts.sample_rate / kNominalRate, kHighestRatio);
        voice.free_at = seconds_ + facts.samples / (ratio * settings_.output_rate);
    }

    if (command.velocity == 0)
    {
        return;
    }

    int pan_shift = 0;
    if (drum->random_pan != 0)
    {
        const int value = static_cast<int>(random_ >> 8 & 0x3F) + static_cast<int>(random_ << 4 & 0xC0);
        const int shift = drum->random_pan * value >> 8;
        pan_shift = ((random_ >> 7) + (random_ >> 2) & 1) != 0 ? shift : -shift;
    }

    if (drum->random_volume != 0 || drum->random_reverb != 0)
    {
        recorder_.Approximate("a drum's random volume or reverb");
    }

    SbnPresetKey key;
    key.drums = true;
    key.fine_cents = track.coarse + track.fine;
    key.cuts = track.polyphony < kFirstSharedPolyphony;
    out_.drum_keys.insert(drum_key);

    Performance& performance = out_.performance;
    track.started = true;
    SendLevels(track);
    recorder_.SendPreset(performance, track.output, tick_, static_cast<uint32_t>(midi::PresetIndex(out_.presets, key)));
    recorder_.SendSetting(performance, track.output, tick_, midi::EventKind::kPan,
                          static_cast<uint8_t>(std::clamp(kCentrePan + pan_shift, 0, kHighestPan)));
    recorder_.SendSetting(performance, track.output, tick_, midi::EventKind::kReverb, drum->reverb);
    recorder_.SendPitch(performance, track.output, tick_, static_cast<float>(tune) / kCentsPerKey);
    voice.key = static_cast<uint8_t>(drum_key);
    voice.midi_track = track.output.midi_track;
    voice.note = recorder_.NoteOn(performance, track.output, tick_, voice.key, VelocityOf(command.velocity));
    voice.sounding = true;
}

void Performer::StartNote(Track& track, const n64::BgmCommand& command, int& scan)
{
    if (track.muted)
    {
        return;
    }

    const std::optional<std::size_t> index = AcquireVoice(track, scan);
    if (!index)
    {
        return;
    }

    Voice& voice = voices_.at(*index);
    voice.length = command.length;

    // A note this short starts no voice. It has still cut the one before.
    if (command.length <= 1)
    {
        return;
    }

    if (track.drums)
    {
        StartDrumNote(track, command, voice);
    }
    else
    {
        StartInstrumentNote(track, command, voice);
    }
}

// The command 0xF5. It sets the track's levels, pan and tuning from an instrument, and keeps the track's random pan.
void Performer::UseInstrument(Track& track, int index)
{
    const n64::BgmInstrument* instrument = nullptr;
    const auto number = static_cast<std::size_t>(index);
    if (number < kSongInstruments)
    {
        instrument = number < file_.instruments.size() ? &file_.instruments[number] : nullptr;
    }
    else if (number - kSongInstruments < prg_.size())
    {
        instrument = &prg_[number - kSongInstruments];
    }

    if (instrument == nullptr)
    {
        track.bank_patch.reset();
        return;
    }

    track.bank_patch = instrument->bank_patch;
    track.instrument_volume = int64_t{instrument->volume & kLevelMask} << kFractionBits;
    track.pan = static_cast<uint8_t>(instrument->pan) & kLevelMask;
    track.reverb = instrument->reverb & kLevelMask;
    track.coarse = instrument->coarse_tune * kCentsPerKey;
    track.fine = instrument->fine_tune;
}

void Performer::RunSpecial(Track& track, const n64::BgmCommand& command)
{
    const uint8_t type = command.args[0];
    const uint8_t first = command.args[1];
    const uint8_t second = command.args[2];
    switch (type)
    {
    case kSpecialStereoDelay:
        recorder_.Approximate("a song's delay of one stereo side");
        break;

    case kSpecialSeekCustom:
        custom_writing_ = first;
        if (first != 0 && first <= kCustomEnvelopes)
        {
            custom_.at(first - std::size_t{1}).fill(kCustomEmpty);
            custom_write_.at(first - std::size_t{1}) = 0;
        }
        else
        {
            custom_writing_ = 0;
        }
        break;

    case kSpecialWriteCustom:
        if (custom_writing_ != 0)
        {
            const auto index = static_cast<std::size_t>(custom_writing_ - 1);
            if (custom_write_.at(index) + std::size_t{1} < kCustomSteps)
            {
                const uint8_t time = first < settings_.custom_times.size() ? settings_.custom_times[first] : first;
                custom_.at(index).at(custom_write_.at(index)) = static_cast<uint16_t>(time << 8 | second);
                custom_write_.at(index)++;
            }
        }
        break;

    case kSpecialUseCustom:
        track.custom_press = first <= kCustomEnvelopes ? first : 0;
        break;

    case kSpecialSound:
        recorder_.Approximate("a sound effect that a song starts");
        break;

    case kSpecialProximity:
        if (first != 0)
        {
            track.proximity_full = first;
            track.proximity_near = second;
        }
        else if (track.proximity_changed)
        {
            track.proximity_changed = false;
            for (Track& other : tracks_)
            {
                const int level = settings_.proximity == kFullLevel ? other.proximity_full : other.proximity_near;
                if (level != 0)
                {
                    other.proximity_changed = false;
                    other.proximity = level;
                    if (other.active && other.started)
                    {
                        SendLevels(other);
                    }
                }
            }
        }
        break;

    default:
        break;
    }
}

void Performer::FadeInstrument(Track& track, int time, int level)
{
    const int64_t target = int64_t{level & kLevelMask} << kFractionBits;
    if (target != track.instrument_volume)
    {
        const int ticks = std::max(time, 1);
        track.fade_ticks = ticks;
        track.fade_target = target;
        track.fade_step = (target - track.instrument_volume) / ticks;
    }
}

void Performer::ApplyTrackLevels()
{
    const std::span<const uint8_t> list = settings_.track_levels;
    for (std::size_t i = 0; i + 1 < list.size() && list[i] != 0 && list[i] <= tracks_.size(); i += 2)
    {
        Track& track = tracks_[list[i] - std::size_t{1}];
        if (track.active)
        {
            FadeInstrument(track, kTrackLevelsFadeTicks, list[i + 1]);
        }
    }
}

void Performer::RunCommand(Track& track, const n64::BgmCommand& command)
{
    const auto volume_of = [](int value) -> int64_t
    {
        return int64_t{value & kLevelMask} << kFractionBits;
    };

    switch (command.op)
    {
    case n64::BgmOp::kTempo:
        {
            const int64_t limit = interval_ / kStepsPerBeatsPerMinute;
            tempo_fade_ticks_ = 0;
            SetTempo(std::clamp<int64_t>(WordAt(command, 0), 1, limit) * 100);
            break;
        }

    case n64::BgmOp::kMasterVolume:
        // The command ends a running fade of the master volume in the game's player too.
        master_volume_ = volume_of(command.args[0]);
        master_fade_ticks_ = 0;
        SendAllLevels();
        break;

    case n64::BgmOp::kMasterTranspose:
        master_shift_ = static_cast<int8_t>(command.args[0]) * kCentsPerKey;
        break;

    case n64::BgmOp::kTempoFade:
        {
            const int64_t limit = interval_ / kStepsPerBeatsPerMinute;
            const int time = std::max(WordAt(command, 0), 1);
            tempo_fade_ticks_ = time;
            tempo_fade_target_ = std::clamp<int64_t>(WordAt(command, 2), 1, limit) * 100;
            tempo_fade_step_ = (tempo_fade_target_ - tempo_) / time;
            break;
        }

    case n64::BgmOp::kMasterVolumeFade:
        {
            const int time = std::max(WordAt(command, 0), 1);
            master_fade_ticks_ = time;
            master_fade_target_ = volume_of(command.args[2]);
            master_fade_step_ = (master_fade_target_ - master_volume_) / time;
            break;
        }

    case n64::BgmOp::kPatch:
        track.bank_patch = static_cast<uint16_t>(command.args[0] << 8 | command.args[1]);
        break;

    case n64::BgmOp::kInstrumentVolume:
        track.instrument_volume = volume_of(command.args[0]);
        break;

    case n64::BgmOp::kInstrumentPan:
        track.pan = command.args[0] & kLevelMask;
        track.random_pan = 0;
        if (track.started && !track.drums)
        {
            recorder_.SendSetting(out_.performance, track.output, tick_, midi::EventKind::kPan,
                                  static_cast<uint8_t>(track.pan));
        }
        break;

    case n64::BgmOp::kInstrumentReverb:
        track.reverb = command.args[0] & kLevelMask;
        if (track.started && !track.drums)
        {
            recorder_.SendSetting(out_.performance, track.output, tick_, midi::EventKind::kReverb,
                                  static_cast<uint8_t>(track.reverb));
        }
        break;

    case n64::BgmOp::kTrackVolume:
        track.volume = command.args[0] & kLevelMask;
        break;

    case n64::BgmOp::kCoarseTune:
        track.coarse = static_cast<int8_t>(command.args[0]) * kCentsPerKey;
        break;

    case n64::BgmOp::kFineTune:
        track.fine = static_cast<int8_t>(command.args[0]);
        break;

    case n64::BgmOp::kTrackDetune:
        track.detune = static_cast<int16_t>(WordAt(command, 0));
        if (track.started)
        {
            recorder_.SendPitch(out_.performance, track.output, tick_, static_cast<float>(track.detune) / kCentsPerKey);
        }
        break;

    case n64::BgmOp::kTremolo:
        track.tremolo_delay = command.args[0];
        track.tremolo_rate = command.args[1];
        track.tremolo_depth = command.args[2];
        break;

    case n64::BgmOp::kTremoloRate:
        track.tremolo_rate = command.args[0];
        break;

    case n64::BgmOp::kTremoloDepth:
        track.tremolo_depth = command.args[0];
        break;

    case n64::BgmOp::kTremoloStop:
        track.tremolo_depth = 0;
        break;

    case n64::BgmOp::kRandomPan:
        track.pan = command.args[0] & kLevelMask;
        track.random_pan = command.args[1] & kLevelMask;
        break;

    case n64::BgmOp::kInstrument:
        UseInstrument(track, command.args[0]);
        break;

    case n64::BgmOp::kInstrumentVolumeFade:
        FadeInstrument(track, WordAt(command, 0), command.args[2]);
        break;

    case n64::BgmOp::kBranch:
        {
            const n64::BgmBranchTarget target =
                n64::ReadBgmBranch(file_.Bytes(), command, static_cast<uint8_t>(settings_.mix));
            track.cursor.branch_end = track.cursor.position;
            track.cursor.position = target.position;
            track.drums = target.drums;
            track.coarse = 0;
            track.fine = 0;
            track.custom_press = 0;
            track.detune = 0;
            track.tremolo_depth = 0;
            track.fade_ticks = 0;
            track.random_pan = 0;
            if (track.proximity_changed)
            {
                track.proximity_changed = false;
                track.proximity = settings_.proximity;
            }
            break;
        }

    case n64::BgmOp::kSpecial:
        RunSpecial(track, command);
        break;

    default:
        break;
    }

    if (track.started)
    {
        SendLevels(track);
    }
}

void Performer::RunVoices(Track& track)
{
    if (track.muted)
    {
        return;
    }

    for (int index = track.first_voice; index < track.last_voice; index++)
    {
        Voice& voice = voices_.at(static_cast<std::size_t>(index));
        CheckDone(voice);
        if (!voice.busy)
        {
            continue;
        }

        if (!voice.pending && voice.length > 0)
        {
            voice.length--;
            if (voice.length == 0)
            {
                ReleaseVoice(voice);
            }
        }

        voice.pending = false;
    }
}

bool Performer::RunTracks()
{
    if (!track_levels_applied_ && seconds_ >= kTrackLevelsSeconds)
    {
        track_levels_applied_ = true;
        ApplyTrackLevels();
    }

    if (tempo_fade_ticks_ != 0)
    {
        tempo_fade_ticks_--;
        tempo_ = tempo_fade_ticks_ == 0 ? tempo_fade_target_ : tempo_ + tempo_fade_step_;
        SetTempo(tempo_);
    }

    if (master_fade_ticks_ != 0)
    {
        master_fade_ticks_--;
        master_volume_ = master_fade_ticks_ == 0 ? master_fade_target_ : master_volume_ + master_fade_step_;
        SendAllLevels();
    }

    bool finished = false;
    for (Track& track : tracks_)
    {
        if (!track.active)
        {
            continue;
        }

        if (track.fade_ticks != 0)
        {
            track.fade_ticks--;
            track.instrument_volume =
                track.fade_ticks == 0 ? track.fade_target : track.instrument_volume + track.fade_step;
            if (track.started)
            {
                SendLevels(track);
            }
        }

        track.delay--;
        if (track.delay <= 0)
        {
            int scan = track.first_voice;
            while (track.delay == 0)
            {
                const n64::BgmCommand command = n64::ReadBgmCommand(file_.Bytes(), track.cursor);
                if (command.op == n64::BgmOp::kEnd)
                {
                    finished = true;
                    break;
                }

                if (command.op == n64::BgmOp::kWait)
                {
                    track.delay = command.length;
                }
                else if (command.op == n64::BgmOp::kNote)
                {
                    StartNote(track, command, scan);
                }
                else if (command.op != n64::BgmOp::kReturn)
                {
                    RunCommand(track, command);
                }
            }
        }

        RunVoices(track);
    }

    return finished;
}

SbnPerformance Performer::Run()
{
    Performance& performance = out_.performance;
    performance.timebase = settings_.ticks_per_beat;
    SetTempo(tempo_);

    composition_offset_ = file_.CompositionOffset(settings_.variation);
    commands_ = file_.Composition(composition_offset_);
    for (std::size_t i = 0; i < commands_.size(); i++)
    {
        if (commands_[i].Op() == n64::BgmCompositionOp::kStartLoop)
        {
            labels_.at(commands_[i].Label()) = i + 1;
        }
    }

    bool fetch = true;
    bool cut = false;
    while (!ended_)
    {
        AdvanceClock();
        int passes = 0;
        do
        {
            if (fetch)
            {
                ReadComposition();
            }

            if (ended_)
            {
                break;
            }

            fetch = RunTracks();
            passes++;
        } while (fetch && passes < kMostPassesInATick);

        if (ended_)
        {
            break;
        }

        tick_++;
        if (seconds_ > kLongestSeconds)
        {
            recorder_.Approximate("a song cut at " + std::to_string(static_cast<int>(kLongestSeconds)) + " seconds");
            cut = true;
            break;
        }
    }

    for (Voice& voice : voices_)
    {
        ResetVoice(voice);
    }

    const uint32_t end_tick = tick_;
    if (loop_ && !cut)
    {
        midi::EndPassAt(performance, end_tick);
        performance.loop = midi::Performance::Loop{loop_->first, end_tick};
        performance.loops = options_.loops;
    }

    performance.end_tick = end_tick;
    performance.seconds = seconds_;
    performance.approximations = recorder_.Approximations();
    performance.warnings = recorder_.Warnings();

    return std::move(out_);
}

} // namespace

SbnPerformance PerformSbn(const n64::BgmFile& file, std::span<const n64::BgmDrum> per,
                          std::span<const n64::BgmInstrument> prg, const SbnFacts& facts, const SbnSettings& settings,
                          const PerformOptions& options)
{
    Performer performer(file, per, prg, facts, settings, options);

    return performer.Run();
}

} // namespace ultralib::sequence_export::n64
