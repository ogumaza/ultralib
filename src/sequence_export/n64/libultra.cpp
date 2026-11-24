// SPDX-License-Identifier: MIT

#include "ultralib/sequence_export/n64/libultra.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <numbers>
#include <numeric>
#include <optional>
#include <set>
#include <utility>
#include <vector>

#include "ntdlib/sequence_export/passes.h"
#include "seqlib/midi/recorder.h"
#include "xsflib/binary.h"

#include "rsp_audio.h"

namespace ultralib::sequence_export::n64
{

namespace
{

namespace n64 = ultralib::n64;
namespace midi = seqlib::midi;

// MIDI's status bytes.
constexpr uint8_t kStatusMask = 0xF0;
constexpr uint8_t kChannelMask = 0x0F;
constexpr uint8_t kNoteOff = 0x80;
constexpr uint8_t kNoteOn = 0x90;
constexpr uint8_t kPolyPressure = 0xA0;
constexpr uint8_t kControlChange = 0xB0;
constexpr uint8_t kProgramChange = 0xC0;
constexpr uint8_t kChannelPressure = 0xD0;
constexpr uint8_t kPitchBend = 0xE0;

// The controllers that the sequence players act on.
constexpr uint8_t kControlVolume = 7;
constexpr uint8_t kControlPan = 10;
constexpr uint8_t kControlSustain = 64;
constexpr uint8_t kControlEffect = 91;

// The controllers that only Ambrella's player acts on. Its loop's controllers go by the code at 0x800055C4.
constexpr uint8_t kControlWheel = 1;
constexpr uint8_t kControlSweepTime = 5;
constexpr uint8_t kControlExpression = 11;
constexpr uint8_t kControlSweep = 65;
constexpr uint8_t kControlLoopStart = 102;
constexpr uint8_t kControlLoopEnd = 103;

// The controllers that only HAL's player acts on, and their values at the start. The code at 0x8002F0A4 reads them.
constexpr uint8_t kControlHalChannelScale = 8;
constexpr uint8_t kControlHalPlayerScale = 21;
constexpr int kHalChannelScale = 127;
constexpr int kHalPlayerScale = 120;
constexpr double kHalChannelScaleFull = 128.0;

constexpr int kChannels = 16;
constexpr int kPercussionChannel = 9;

// The tempo that a sequence starts with, in microseconds a quarter note.
constexpr float kDefaultTempo = 500000.0F;

// The count in a loop's end that loops for ever.
constexpr uint8_t kLoopForever = 0xFF;

// The volume at the top of the player's scale, and the voice's volume when every factor stands at its top: the
// player's fixed-point product. The mixer squares it, out of 32768.
constexpr int kFullPlayerVolume = 0x7FFF;
constexpr double kFullVoiceVolume = 31507.0 / 32768.0;

// Ambrella's player, read at 0x80006254 and 0x80007420 in Hey You, Pikachu!. A voice's volume at the top of every
// factor: a controller of 127 scales by 16256/16384, and the envelope's level by 0xF000/0x10000. A channel starts at a
// volume of 100 and at the bank's percussion, with 64 as its last key (the reset at 0x80005F30). A bend covers 200
// cents. A note's pitch takes the key map's detune away.
constexpr double kAmbrellaFullVoiceVolume = 29769.0 / 32768.0;
constexpr int kAmbrellaBendCents = 200;
constexpr uint8_t kAmbrellaChannelVolume = 100;
constexpr int kAmbrellaFirstKey = 64;

// The player's update lasts this long. Its oscillators step through a period in about 16.03 / rate seconds. The
// conversion takes 16. The bank's depths scale CC1's value over 127 by 127 / 128 twice over.
constexpr double kAmbrellaUpdateSeconds = 0.00575;
constexpr double kAmbrellaLfoUnits = 16.0;
constexpr double kAmbrellaWheelVibrato = 127.0 * 127.0 / (128.0 * 128.0);
constexpr double kAmbrellaWheelTremolo = 127.0 * 255.0 / (128.0 * 256.0 * 256.0);

// A sweep moves this many cents, over CC5's value and 1, in each update. The file holds a step for two of them.
constexpr double kAmbrellaSweepUnits = 768.0;
constexpr double kAmbrellaSweepStepUpdates = 2.0;

// A ramp to nothing ends at an amplitude of 1/32768: 90.3 dB down. A voice with an attack starts there.
constexpr double kFloorDb = 90.31;

// The velocity that the envelope's times are fitted at. A ramp to nothing takes the same time from any level. A quieter
// note falls more slowly in decibels. SoundFont's envelopes can't follow that.
constexpr double kNominalVelocity = 100.0;

// Each channel's gain at its centre pan, in the mixer's sin/cos law.
constexpr double kCentreDb = -3.01;

// The places where a decay's fit meets a straight line in amplitude. Each is a share of the line's time.
constexpr double kLineRampEarlyPlace = 0.5;
constexpr double kLineRampLatePlace = 0.9;

// The report's line for a note whose decay or release the fit of straight ramps approximates (ApplyLineRamps).
constexpr const char* kLineRampsFitted = "straight-line decays and releases fitted to their top 20 dB";

// A tick at an end in seconds that a conversion back from seconds overshoots by up to this share of a tick is that
// tick.
constexpr double kTickRounding = 1e-6;

// A release in a straight line of amplitude falls like a line in decibels that takes this many times its time for 100
// dB. That line falls 20 dB in the release's time. A longer one follows the top of the ramp more closely and adds a
// tail that the game doesn't have. A shorter one falls too soon.
constexpr double kLineReleaseTo100Db = 5.0;

// The resampler's greatest pitch: just under an octave up.
constexpr int kOctaveCents = 1200;

// The SDK's oscillators. A sine has 259 - rate steps and the others 256 - rate. Each step lasts 16 ms, and a delay's
// unit 16.384 ms.
constexpr int kTremoloSine = 1;
constexpr int kVibratoSine = 128;
constexpr int kFirstVibrato = 128;
constexpr int kSineSteps = 259;
constexpr int kOtherSteps = 256;
constexpr double kOscillatorStepMs = 16.0;
constexpr double kOscillatorDelayMs = 16.384;

// The base of an oscillator's depth in cents: a vibrato's depth is this to the power of the bank's depth.
constexpr double kVibratoDepthBase = 1.03099303;

// The rules of the main loop's passes (ntdlib/sequence_export/passes.h). A program change sends the channel's volume
// and pan at once, and the program goes out with the channel's next note. The settings that RestoreAtLoopStart sends
// again therefore go ahead of the presets. The performer sends each track's notes to one part, with no part switches.
const ntdlib::sequence_export::LoopRules kLoopRules{.seeded = {}, .preset_with_settings = false};

// A setting of a channel's track: the channel and the kind of event.
using ChannelSetting = std::pair<int, midi::EventKind>;

// A main loop that starts at tick 0 contains the settings that the performer sends there for the game's set-up of each
// channel. The game sets them once. A MIDI player that loops on the markers sends them again at each repeat, and a note
// held across the loop's end plays with them where the game keeps the loop end's value. Moves each setting of `set_up`
// to the sequence's first value of it, from `first_sends`, where no note plays with it before that value and the loop
// ends with another value. The recorder leaves out a first value that equals the set-up value. The moved event replaces
// it. A note of the loop with the set-up's value plays with the loop end's value in the game's next pass, and the pass
// test then moves the loop to a later pass. Only a first pass that the time limit leaves untested keeps such a setting
// at tick 0.
void MoveSetUpInLoop(Performance& performance, const std::set<ChannelSetting>& set_up,
                     const std::map<ChannelSetting, uint32_t>& first_sends)
{
    if (!performance.loop || performance.loop->start != 0)
    {
        return;
    }

    const std::map<ntdlib::sequence_export::SettingKey, midi::TrackEvent> at_end =
        ntdlib::sequence_export::SettingsAt(performance, performance.loop->end);
    for (const ChannelSetting& setting : set_up)
    {
        // A setting that the sequence never sends keeps the set-up value up to the end of the loop. A first value at
        // tick 0 stays where it is.
        const auto first = first_sends.find(setting);
        if (first == first_sends.end())
        {
            continue;
        }

        const auto [channel, kind] = setting;
        const uint32_t tick = first->second;
        std::vector<midi::TrackEvent>& events = performance.tracks[static_cast<std::size_t>(channel)];
        const auto of_kind = [kind](const midi::TrackEvent& e)
        {
            return e.kind == kind;
        };
        const auto set = std::ranges::find_if(events, of_kind);

        const auto heard = [tick](const midi::TrackEvent& e)
        {
            return e.kind == midi::EventKind::kNoteOn && e.tick < tick;
        };
        if (set == events.end() || set->tick != 0 || std::ranges::any_of(events, heard))
        {
            continue;
        }

        const auto end = at_end.find({channel, set->part, kind});
        if (end == at_end.end() || ntdlib::sequence_export::SameSetting(end->second, *set))
        {
            continue;
        }

        // The recorder puts a setting ahead of the notes and programs at the same tick. The moved setting goes in the
        // same place, at the tick of the first value.
        midi::TrackEvent moved = *set;
        moved.tick = tick;
        events.erase(set);
        const auto sent_there = [&moved](const midi::TrackEvent& e)
        {
            return e.tick == moved.tick && e.kind == moved.kind && e.part == moved.part;
        };
        const auto after = [&moved](const midi::TrackEvent& e)
        {
            return e.tick > moved.tick ||
                   (e.tick == moved.tick && (e.kind == midi::EventKind::kNoteOn || e.kind == midi::EventKind::kPreset));
        };
        if (std::ranges::none_of(events, sent_there))
        {
            events.insert(std::ranges::find_if(events, after), moved);
        }
    }
}

// The envelope that `player` plays for a sound with `envelope`. Ambrella's player starts a note at its top and only
// acts on a fall to nothing.
n64::BankEnvelope PlayedEnvelope(n64::BankEnvelope envelope, LibultraPlayer player)
{
    if (player == LibultraPlayer::kAmbrella)
    {
        const bool falls = envelope.decay_time != 0 && envelope.decay_volume == 0;
        envelope.attack_time = 0;
        envelope.attack_volume = 127;
        envelope.decay_time = falls ? envelope.decay_time : 0;
        envelope.decay_volume = falls ? 0 : 127;
    }

    return envelope;
}

// True if the fit of straight ramps (ApplyLineRamps) approximates a ramp of `envelope`: a decay that falls, or a
// release. Its attack is exact.
bool FitsLineRamps(const n64::BankEnvelope& envelope)
{
    return (envelope.decay_time > 0 && envelope.decay_volume < envelope.attack_volume) || envelope.release_time > 0;
}

// A channel's sound in a player. A voice whose sound has no release ends at once.
struct Voice
{
    int key = 0;
    uint32_t note = 0;
    uint8_t part = 0;
    bool ends_at_once = false;
};

// A loop that a track repeats for ever: the ticks of its start and its end.
struct ForeverLoop
{
    uint32_t start = 0;
    uint32_t end = 0;
};

// The place of a track in its song.
struct TrackState
{
    std::size_t index = 0;
    uint32_t next_tick = 0;
    bool ended = false;
    std::optional<ForeverLoop> forever;
    uint32_t end_tick = 0;
    std::map<std::size_t, uint8_t> loop_counts;
    std::map<std::size_t, uint32_t> loop_ticks;

    // Where a loop of Ambrella's player starts: the next event, and its tick.
    std::optional<std::size_t> loop_start_index;
    uint32_t loop_start_tick = 0;
};

// A note-off that a compressed note on schedules.
struct PendingOff
{
    int channel = 0;
    int key = 0;
};

class Performer
{
public:
    Performer(const LibultraSong& song, const n64::BankFile& file, const n64::Bank& bank,
              const LibultraSettings& settings, const PerformOptions& options)
        : song_(song), file_(file), bank_(bank), settings_(settings), options_(options)
    {
    }

    LibultraPerformance Run();

private:
    void SetTempo(uint32_t tick, float tempo);
    double SecondsAt(uint32_t tick) const;
    uint32_t TickAt(double seconds) const;
    void SendSetting(int channel, uint32_t tick, midi::EventKind kind, uint8_t value);
    void SetInstrument(int channel, int32_t instrument, uint32_t tick);
    void HandleEvent(int track, const n64::MidiEvent& event, uint32_t tick);
    void HandleChannelMessage(const n64::MidiEvent& event, uint32_t tick);
    void HandleNoteOn(int channel, int key, int velocity, uint32_t duration, uint32_t tick);
    void HandleNoteOff(int channel, int key, uint32_t tick);
    bool HandleLoopEnd(TrackState& state, const n64::MidiEvent& event, uint32_t tick);
    void PlanLoop();
    bool PlaysAnotherPass();
    std::function<bool(uint32_t, uint32_t)> PlayOn(uint32_t until);
    void Play();
    double ReleaseMs(int32_t instrument) const;
    void Finish(uint32_t tick);
    void SendExpression(int channel, uint32_t tick);
    void SendPitch(int channel, uint32_t tick);
    void StartSweep(int channel, int key, uint32_t tick);
    void RunSweeps(uint32_t until_tick);
    double TicksPerUpdate(uint32_t tick) const;

    const LibultraSong& song_;
    const n64::BankFile& file_;
    const n64::Bank& bank_;
    const LibultraSettings& settings_;
    const PerformOptions& options_;

    LibultraPerformance out_;
    midi::Recorder recorder_;
    std::array<midi::TrackOutput, kChannels> outputs_;
    std::array<int32_t, kChannels> instruments_{};
    int hal_player_scale_ = kHalPlayerScale;
    std::array<int, kChannels> hal_channel_scales_{};
    std::array<int, kChannels> bend_ranges_{};
    std::array<std::vector<Voice>, kChannels> voices_;

    // What Ambrella's player holds for each channel: CC11, the effect's share, the bend in cents, CC5 and CC65, the
    // last key, and a sweep from that key: the cents that are left and the next step's tick.
    std::array<int, kChannels> expressions_{};
    std::array<int, kChannels> effects_{};
    std::array<double, kChannels> bend_cents_{};
    std::array<int, kChannels> sweep_times_{};
    std::array<bool, kChannels> sweeps_on_{};
    std::array<int, kChannels> last_keys_{};
    std::array<double, kChannels> sweep_cents_{};
    std::array<uint32_t, kChannels> sweep_ticks_{};
    std::multimap<uint32_t, PendingOff> pending_offs_;
    std::vector<TrackState> tracks_;

    // The tempo map.
    struct TempoSegment
    {
        uint32_t tick = 0;
        double seconds = 0.0;
        double uspt = 0.0;
    };

    std::vector<TempoSegment> tempo_;

    // Where the performance stops, once every track has ended or found its loop: the end of the main loop's pass that
    // plays now. A copy that plays on (PlayOn) stops at the tick it's given.
    std::optional<uint32_t> stop_tick_;
    uint32_t tick_ = 0;
    double last_release_end_ = 0.0;

    // The main loop: the start of its first pass, the length of a pass, and the passes counted and marked. A copy plays
    // on without counting passes. `played_on_` is the last copy's performance. It holds the pass after the marked one.
    uint32_t loop_start_ = 0;
    uint32_t loop_length_ = 0;
    ntdlib::sequence_export::MainLoopPasses main_loop_{std::vector<ntdlib::sequence_export::LoopChoice>{}};
    bool copy_ = false;
    std::shared_ptr<const Performance> played_on_;

    // The settings that the performer sends at tick 0 for the game's set-up of each channel, and the tick of the
    // sequence's first value of each setting.
    bool setting_up_ = true;
    std::set<ChannelSetting> set_up_;
    std::map<ChannelSetting, uint32_t> first_sends_;
};

void Performer::SetTempo(uint32_t tick, float tempo)
{
    // The player's microseconds a tick are a whole number, in single precision.
    const float ticks_per_quarter = 1.0F / static_cast<float>(song_.division);
    const double uspt = std::max(static_cast<int32_t>(tempo * ticks_per_quarter), 1) / settings_.rate;
    tempo_.push_back({tick, SecondsAt(tick), uspt});
    recorder_.SetTempo(out_.performance, tick, 60000000.0 / (uspt * song_.division));
}

double Performer::SecondsAt(uint32_t tick) const
{
    if (tempo_.empty())
    {
        return 0.0;
    }

    const TempoSegment& last = tempo_.back();
    return last.seconds + (static_cast<double>(tick) - last.tick) * last.uspt / 1000000.0;
}

uint32_t Performer::TickAt(double seconds) const
{
    // The segment that holds `seconds`: the last one that starts at or before it.
    const auto after = std::ranges::upper_bound(tempo_, seconds, {}, &TempoSegment::seconds);
    const TempoSegment& segment = after == tempo_.begin() ? tempo_.front() : *std::prev(after);
    const double ticks = (seconds - segment.seconds) * 1000000.0 / segment.uspt;

    return segment.tick + static_cast<uint32_t>(std::max(std::ceil(ticks - kTickRounding), 0.0));
}

void Performer::SendSetting(int channel, uint32_t tick, midi::EventKind kind, uint8_t value)
{
    if (setting_up_)
    {
        set_up_.insert({channel, kind});
    }
    else
    {
        first_sends_.try_emplace({channel, kind}, tick);
    }

    recorder_.SendSetting(out_.performance, outputs_[static_cast<std::size_t>(channel)], tick, kind, value);
}

void Performer::SetInstrument(int channel, int32_t instrument, uint32_t tick)
{
    const n64::BankInstrument& object = file_.instruments[static_cast<std::size_t>(instrument)];
    instruments_[static_cast<std::size_t>(channel)] = instrument;
    if (settings_.player == LibultraPlayer::kAmbrella)
    {
        return;
    }

    bend_ranges_[static_cast<std::size_t>(channel)] = object.bend_range;

    SendSetting(channel, tick, midi::EventKind::kVolume, object.volume);
    SendSetting(channel, tick, midi::EventKind::kPan, object.pan);
}

double Performer::ReleaseMs(int32_t instrument) const
{
    double longest = 0.0;
    for (const int32_t sound : file_.instruments[static_cast<std::size_t>(instrument)].sounds)
    {
        const int32_t envelope = file_.sounds[static_cast<std::size_t>(sound)].envelope;
        if (envelope != n64::kNoObject)
        {
            longest = std::max(longest, file_.envelopes[static_cast<std::size_t>(envelope)].release_time / 1000.0);
        }
    }

    return longest;
}

// The binary search of the players' sound lookup. A note that it doesn't find plays no sound.
int32_t LookupSound(const n64::BankFile& file, const n64::BankInstrument& instrument, int key, int velocity)
{
    int low = 1;
    int high = static_cast<int>(instrument.sounds.size());
    while (high >= low)
    {
        const int middle = (low + high) / 2;
        const int32_t sound = instrument.sounds[static_cast<std::size_t>(middle - 1)];
        const int32_t key_map = file.sounds[static_cast<std::size_t>(sound)].key_map;
        if (key_map == n64::kNoObject)
        {
            return n64::kNoObject;
        }

        const n64::BankKeyMap& map = file.key_maps[static_cast<std::size_t>(key_map)];
        if (key >= map.key_min && key <= map.key_max && velocity >= map.velocity_min && velocity <= map.velocity_max)
        {
            return sound;
        }

        if (key < map.key_min || (velocity < map.velocity_min && key <= map.key_max))
        {
            high = middle - 1;
        }
        else
        {
            low = middle + 1;
        }
    }

    return n64::kNoObject;
}

void Performer::SendExpression(int channel, uint32_t tick)
{
    // The mixer takes the send's share from the dry level, on the sin/cos law. The controller acts squared.
    const std::size_t index = static_cast<std::size_t>(channel);
    const double dry = DryShare(effects_[index]);
    const double expression = settings_.player == LibultraPlayer::kHal
                                  ? hal_player_scale_ * hal_channel_scales_[index] / kHalChannelScaleFull
                                  : expressions_[index];
    SendSetting(channel, tick, midi::EventKind::kExpression,
                static_cast<uint8_t>(std::lround(expression * std::sqrt(dry))));
}

void Performer::SendPitch(int channel, uint32_t tick)
{
    const std::size_t index = static_cast<std::size_t>(channel);
    recorder_.SendPitch(out_.performance, outputs_[index], tick,
                        static_cast<float>((bend_cents_[index] + sweep_cents_[index]) / 100.0));
}

double Performer::TicksPerUpdate(uint32_t tick) const
{
    const TempoSegment& segment =
        *std::prev(std::ranges::upper_bound(tempo_, tick, {}, [](const TempoSegment& s) { return s.tick; }));

    return kAmbrellaUpdateSeconds * 1000000.0 / segment.uspt;
}

// A note takes over from the channel's last key. The player sweeps each voice, and a channel's other voices move along
// here. The channels that sweep play one note at a time.
void Performer::StartSweep(int channel, int key, uint32_t tick)
{
    const std::size_t index = static_cast<std::size_t>(channel);
    if (sweeps_on_[index])
    {
        sweep_cents_[index] = (last_keys_[index] - key) * 100.0;
        sweep_ticks_[index] = tick;
        SendPitch(channel, tick);
    }

    last_keys_[index] = key;
}

void Performer::RunSweeps(uint32_t until_tick)
{
    for (int channel = 0; channel < kChannels; channel++)
    {
        const std::size_t index = static_cast<std::size_t>(channel);
        while (sweep_cents_[index] != 0.0)
        {
            const double updates = std::max(kAmbrellaSweepStepUpdates * TicksPerUpdate(sweep_ticks_[index]), 1.0);
            const uint32_t next = sweep_ticks_[index] + static_cast<uint32_t>(std::lround(updates));
            if (next > until_tick)
            {
                break;
            }

            const double step = kAmbrellaSweepStepUpdates * std::floor(kAmbrellaSweepUnits / (sweep_times_[index] + 1));
            const double left = std::max(std::abs(sweep_cents_[index]) - step, 0.0);
            sweep_cents_[index] = std::copysign(left, sweep_cents_[index]);
            sweep_ticks_[index] = next;
            SendPitch(channel, next);
        }
    }
}

void Performer::HandleNoteOn(int channel, int key, int velocity, uint32_t duration, uint32_t tick)
{
    const std::size_t index = static_cast<std::size_t>(channel);
    const n64::BankInstrument& instrument = file_.instruments[static_cast<std::size_t>(instruments_[index])];
    const int32_t sound = LookupSound(file_, instrument, key, velocity);
    if (sound == n64::kNoObject)
    {
        return;
    }

    // The Ambrella player updates the last key only for a note that finds a sound.
    if (settings_.player == LibultraPlayer::kAmbrella)
    {
        StartSweep(channel, key, tick);
    }

    // CC91 goes out before a part's first note, at the channel's effect send: 0 until the sequence sets one, and 0
    // where the game has no effect bus. A later note sends nothing more unless the send has changed. A synth that
    // follows General MIDI 2 gives a channel without CC91 a send of 40.
    midi::TrackOutput& output = outputs_[index];
    const int send = settings_.effect == n64::AlEffect::kNone ? 0 : effects_[index];
    SendSetting(channel, tick, midi::EventKind::kReverb, static_cast<uint8_t>(send));

    // A program goes out with its channel's first note. A program that plays no note then stays out of the SoundFont.
    recorder_.SendPreset(out_.performance, output, tick, midi::PresetIndex(out_.instruments, instruments_[index]));
    const uint32_t note =
        recorder_.NoteOn(out_.performance, output, tick, static_cast<uint8_t>(key), static_cast<uint8_t>(velocity));
    const int32_t envelope = file_.sounds[static_cast<std::size_t>(sound)].envelope;
    const bool ends_at_once =
        envelope == n64::kNoObject || file_.envelopes[static_cast<std::size_t>(envelope)].release_time == 0;
    voices_[index].push_back({key, note, output.part, ends_at_once});
    if (settings_.line_ramps && envelope != n64::kNoObject &&
        FitsLineRamps(PlayedEnvelope(file_.envelopes[static_cast<std::size_t>(envelope)], settings_.player)))
    {
        recorder_.Approximate(kLineRampsFitted);
    }

    if (duration != 0)
    {
        pending_offs_.insert({tick + duration, {channel, key}});
    }
}

void Performer::HandleNoteOff(int channel, int key, uint32_t tick)
{
    std::vector<Voice>& voices = voices_[static_cast<std::size_t>(channel)];
    const auto release = [&](const Voice& voice)
    {
        recorder_.NoteOff(out_.performance, static_cast<uint8_t>(channel), tick, voice.part, static_cast<uint8_t>(key),
                          voice.note);
        last_release_end_ = std::max(
            last_release_end_, SecondsAt(tick) + ReleaseMs(instruments_[static_cast<std::size_t>(channel)]) / 1000.0);
    };

    // The Ambrella player walks the voice list from the newest voice (the code at 0x800067BC). It releases every voice
    // of the key. A sound with no release ends its voice at once. The walk then leaves the list.
    if (settings_.player == LibultraPlayer::kAmbrella)
    {
        for (std::size_t place = voices.size(); place-- > 0;)
        {
            if (voices[place].key != key)
            {
                continue;
            }

            const bool ends_walk = voices[place].ends_at_once;
            release(voices[place]);
            voices.erase(voices.begin() + static_cast<std::ptrdiff_t>(place));
            if (ends_walk)
            {
                break;
            }
        }

        return;
    }

    // The other players release the first voice of the key.
    const auto found = std::ranges::find_if(voices, [key](const Voice& voice) { return voice.key == key; });
    if (found != voices.end())
    {
        release(*found);
        voices.erase(found);
    }
}

void Performer::HandleChannelMessage(const n64::MidiEvent& event, uint32_t tick)
{
    const int channel = event.status & kChannelMask;
    const std::size_t index = static_cast<std::size_t>(channel);
    switch (event.status & kStatusMask)
    {
    case kNoteOn:
        if (event.data2 != 0)
        {
            HandleNoteOn(channel, event.data1, event.data2, event.duration, tick);
            break;
        }

        [[fallthrough]];

    case kNoteOff:
        HandleNoteOff(channel, event.data1, tick);
        break;

    case kControlChange:
        if (settings_.player == LibultraPlayer::kAmbrella)
        {
            switch (event.data1)
            {
            case kControlWheel:
                SendSetting(channel, tick, midi::EventKind::kModulation, event.data2);
                break;

            case kControlExpression:
                expressions_[index] = event.data2;
                SendExpression(channel, tick);
                break;

            case kControlSweepTime:
                sweep_times_[index] = event.data2;
                break;

            case kControlSweep:
                sweeps_on_[index] = event.data2 != 0;
                break;

            default:
                break;
            }
        }

        if (settings_.player == LibultraPlayer::kHal)
        {
            if (event.data1 == kControlHalChannelScale)
            {
                hal_channel_scales_[index] = event.data2;
                SendExpression(channel, tick);
            }
            else if (event.data1 == kControlHalPlayerScale)
            {
                hal_player_scale_ = event.data2;
                for (int other = 0; other < kChannels; other++)
                {
                    SendExpression(other, tick);
                }
            }
        }

        switch (event.data1)
        {
        case kControlVolume:
            SendSetting(channel, tick, midi::EventKind::kVolume, event.data2);
            break;

        case kControlPan:
            SendSetting(channel, tick, midi::EventKind::kPan, event.data2);
            break;

        case kControlSustain:
            SendSetting(channel, tick, midi::ControlKind(kControlSustain), event.data2);
            break;

        case kControlEffect:
            effects_[index] = event.data2;
            SendExpression(channel, tick);
            if (settings_.effect != n64::AlEffect::kNone)
            {
                SendSetting(channel, tick, midi::EventKind::kReverb, event.data2);
            }
            break;

        default:
            break;
        }
        break;

    case kProgramChange:
        if (event.data1 < bank_.instruments.size() && bank_.instruments[event.data1] != n64::kNoObject)
        {
            SetInstrument(channel, bank_.instruments[event.data1], tick);
        }
        break;

    case kPitchBend:
        {
            const int bend = (event.data2 << 7) + event.data1 - 8192;
            bend_cents_[index] = bend_ranges_[index] * bend / 8192;
            SendPitch(channel, tick);
            break;
        }

    case kPolyPressure:
    case kChannelPressure:
        recorder_.Approximate("aftertouch left out");
        break;

    default:
        break;
    }
}

bool Performer::HandleLoopEnd(TrackState& state, const n64::MidiEvent& event, uint32_t tick)
{
    if (event.loop_start < 0 || event.payload.size() < 2)
    {
        return false;
    }

    // The first byte holds the count, and the second the count that's left. A count of 0xFF repeats for ever. A count
    // that has run out is set again for the next time the track reaches the loop.
    const std::size_t index = state.index;
    if (!state.loop_counts.contains(index))
    {
        state.loop_counts[index] = event.payload[1];
    }

    uint8_t& left = state.loop_counts[index];
    if (left == 0)
    {
        left = event.payload[0];
        return false;
    }

    const std::size_t start = static_cast<std::size_t>(event.loop_start);
    if (left != kLoopForever)
    {
        left--;
    }
    else if (!state.forever)
    {
        state.forever = ForeverLoop{state.loop_ticks[start], tick};
    }

    // The player goes on from the bytes after the loop's start.
    state.index = start + 1;

    return true;
}

// Once every track has ended or found its loop, the performance repeats. The main loop starts when the last track has
// joined it, and lasts until every track's loop comes round together.
void Performer::PlanLoop()
{
    if (stop_tick_)
    {
        return;
    }

    uint32_t start = 0;
    uint64_t length = 1;
    bool any_loop = false;
    for (const TrackState& state : tracks_)
    {
        if (state.ended)
        {
            start = std::max(start, state.end_tick);
        }
        else if (state.forever)
        {
            any_loop = true;
            start = std::max(start, state.forever->start);
            length = std::lcm(length, static_cast<uint64_t>(state.forever->end - state.forever->start));
        }
        else
        {
            return;
        }
    }

    if (!any_loop)
    {
        return;
    }

    // The pass choice counts up to twice --loops passes, and a copy of the performer plays two more (ChooseSteadyPass).
    const uint64_t passes = 2 * static_cast<uint64_t>(std::max(options_.loops, 1)) + 2;
    if (start + length * passes >= UINT32_MAX || SecondsAt(static_cast<uint32_t>(start + length)) > kLongestSeconds)
    {
        return;
    }

    // The performance stops at the end of each pass, to count it.
    loop_start_ = start;
    loop_length_ = static_cast<uint32_t>(length);
    stop_tick_ = static_cast<uint32_t>(start + length);
    main_loop_.Restart(out_.performance, std::pair<int, uint32_t>{0, start});
}

// Counts the pass of the main loop that ends at the stop tick, and returns true if the performance plays another pass.
// Once the performance has played --loops passes, the pass rules mark the first pass that a MIDI player can repeat as
// the game plays the passes after it (MainLoopPasses::MarkSteadyPass). The performance plays on until --loops passes
// follow the marked one, or until the time limit cuts it.
bool Performer::PlaysAnotherPass()
{
    Performance& performance = out_.performance;
    const uint32_t end = *stop_tick_;
    main_loop_.CountPass(performance, std::pair<int, uint32_t>{0, loop_start_}, end - loop_length_, end);
    if (performance.loops >= options_.loops)
    {
        const auto play_on = [this](uint32_t until)
        {
            return PlayOn(until);
        };
        if (main_loop_.MarkSteadyPass(performance, std::nullopt, options_.loops, play_on))
        {
            return false;
        }
    }

    stop_tick_ = end + loop_length_;

    return true;
}

// Plays a copy of the performer on, up to tick `until`, without counting passes and without the time limit: a pass that
// the limit would cut can still show which pass repeats. Returns the test of whether the copy's pass from a start to an
// end plays as the pass after it (RepeatsAsTheGame). The performer draws no random numbers. The copy's performance
// stays in `played_on_`.
std::function<bool(uint32_t, uint32_t)> Performer::PlayOn(uint32_t until)
{
    Performer copy = *this;
    copy.copy_ = true;
    copy.stop_tick_ = until;
    copy.Play();

    const auto played = std::make_shared<const Performance>(std::move(copy.out_.performance));
    played_on_ = played;

    return [played](uint32_t start, uint32_t end)
    {
        return ntdlib::sequence_export::RepeatsAsTheGame(*played, {}, start, end, kLoopRules);
    };
}

void Performer::Finish(uint32_t tick)
{
    Performance& performance = out_.performance;
    const double sound_end = last_release_end_;

    // A main loop in which no note starts, once every note before it has ended, sounds nothing. It's no loop. The
    // performance ends with its last sound, and what comes after that can't be heard.
    const auto no_voice = [](const std::vector<Voice>& voices)
    {
        return voices.empty();
    };
    const bool silent_loop =
        performance.loop && std::ranges::all_of(voices_, no_voice) && !midi::StartsNote(performance, loop_start_);

    // A note that's still on stops where the performance stops.
    for (int channel = 0; channel < kChannels; channel++)
    {
        for (const Voice& voice : voices_[static_cast<std::size_t>(channel)])
        {
            recorder_.NoteOff(performance, static_cast<uint8_t>(channel), tick, voice.part,
                              static_cast<uint8_t>(voice.key), voice.note);
        }
    }

    if (silent_loop)
    {
        const uint32_t end = TickAt(sound_end);
        midi::EndAt(performance, end);
        performance.DropLoop();
        performance.end_tick = end;
        performance.seconds = sound_end;
    }
    else if (performance.loop)
    {
        // The next pass starts at the end of the last. What it plays stays out, and the notes stop there. The game's
        // pass after the marked one is in the copy that played on: the performance stops before it.
        midi::EndPassAt(performance, tick);
        performance.end_tick = tick;
        performance.seconds = SecondsAt(tick);
        MoveSetUpInLoop(performance, set_up_, first_sends_);
        if (played_on_)
        {
            const ntdlib::sequence_export::PassStart next_pass =
                ntdlib::sequence_export::PassStartAt(*played_on_, {}, performance.loop->end);
            ntdlib::sequence_export::RestoreAtLoopStart(performance, {}, next_pass, kLoopRules);
        }
    }
    else
    {
        // The file ends once the last release has ended.
        const TempoSegment& last = tempo_.back();
        const double seconds = std::max(sound_end, SecondsAt(tick));
        const double ticks = (seconds - last.seconds) * 1000000.0 / last.uspt + last.tick;
        performance.end_tick = std::max(tick, static_cast<uint32_t>(std::ceil(ticks)));
        performance.seconds = seconds;
    }

    performance.approximations = recorder_.Approximations();
    performance.warnings = recorder_.Warnings();
}

void Performer::HandleEvent(int track, const n64::MidiEvent& event, uint32_t tick)
{
    TrackState& state = tracks_[static_cast<std::size_t>(track)];
    state.index++;
    if (event.status == n64::kMidiMeta)
    {
        switch (event.type)
        {
        case n64::kMidiMetaTempo:
            SetTempo(tick, static_cast<float>(event.TempoMicroseconds()));
            break;

        case n64::kMidiMetaEndOfTrack:
            state.ended = true;
            state.end_tick = tick;
            break;

        case n64::kCompressedLoopStart:
            state.loop_ticks[state.index - 1] = tick;
            break;

        case n64::kCompressedLoopEnd:
            state.index--;
            if (!HandleLoopEnd(state, event, tick))
            {
                state.index++;
            }
            break;

        default:
            break;
        }
    }
    else if (settings_.player == LibultraPlayer::kAmbrella && (event.status & kStatusMask) == kControlChange &&
             event.data1 == kControlLoopStart)
    {
        state.loop_start_index = state.index;
        state.loop_start_tick = tick;
    }
    else if (settings_.player == LibultraPlayer::kAmbrella && (event.status & kStatusMask) == kControlChange &&
             event.data1 == kControlLoopEnd && state.loop_start_index)
    {
        // The player ends every note that's on. From the sixth pass on, it plays two passes of every three with a
        // random set of the channels 9 to 16. The loop in the file is the song as it first plays.
        for (int channel = 0; channel < kChannels; channel++)
        {
            while (!voices_[static_cast<std::size_t>(channel)].empty())
            {
                HandleNoteOff(channel, voices_[static_cast<std::size_t>(channel)].front().key, tick);
            }
        }

        state.forever = ForeverLoop{state.loop_start_tick, tick};
        state.index = *state.loop_start_index;
    }
    else if (event.IsChannel())
    {
        HandleChannelMessage(event, tick);
    }

    const std::vector<n64::MidiEvent>& events = song_.tracks[static_cast<std::size_t>(track)];
    if (state.ended || state.index >= events.size())
    {
        state.ended = true;
        state.end_tick = std::max(state.end_tick, tick);
        return;
    }

    state.next_tick = tick + events[state.index].delta;
}

LibultraPerformance Performer::Run()
{
    Performance& performance = out_.performance;
    performance.timebase = static_cast<uint32_t>(song_.division);
    for (int channel = 0; channel < kChannels; channel++)
    {
        outputs_[static_cast<std::size_t>(channel)].midi_track = static_cast<uint8_t>(channel);
    }

    SetTempo(0, kDefaultTempo);

    // Each channel starts on the bank's first instrument, and channel 10 on the bank's percussion.
    const auto first = std::ranges::find_if(bank_.instruments, [](int32_t i) { return i != n64::kNoObject; });
    if (first == bank_.instruments.end())
    {
        throw xsflib::FormatError("the bank has no instruments");
    }

    for (int channel = 0; channel < kChannels; channel++)
    {
        const std::size_t index = static_cast<std::size_t>(channel);
        const bool ambrella = settings_.player == LibultraPlayer::kAmbrella;
        const bool percussion = (ambrella || channel == kPercussionChannel) && bank_.percussion != n64::kNoObject;
        SetInstrument(channel, percussion ? bank_.percussion : *first, 0);
        expressions_[index] = 127;
        hal_channel_scales_[index] = kHalChannelScale;
        if (settings_.player == LibultraPlayer::kHal)
        {
            SendExpression(channel, 0);
        }

        if (ambrella)
        {
            bend_ranges_[index] = kAmbrellaBendCents;
            last_keys_[index] = kAmbrellaFirstKey;
            SendSetting(channel, 0, midi::EventKind::kVolume, kAmbrellaChannelVolume);
        }
    }

    setting_up_ = false;

    tracks_.resize(song_.tracks.size());
    for (std::size_t i = 0; i < song_.tracks.size(); i++)
    {
        if (song_.tracks[i].empty())
        {
            tracks_[i].ended = true;
        }
        else
        {
            tracks_[i].next_tick = song_.tracks[i][0].delta;
        }
    }

    Play();
    Finish(tick_);

    return std::move(out_);
}

void Performer::Play()
{
    Performance& performance = out_.performance;
    while (true)
    {
        // The lowest track with the nearest event goes next. A note's end at the same tick goes before it.
        int next = -1;
        for (int i = 0; i < static_cast<int>(tracks_.size()); i++)
        {
            const TrackState& state = tracks_[static_cast<std::size_t>(i)];
            if (!state.ended && (next < 0 || state.next_tick < tracks_[static_cast<std::size_t>(next)].next_tick))
            {
                next = i;
            }
        }

        const bool stopped =
            next >= 0 && stop_tick_ && tracks_[static_cast<std::size_t>(next)].next_tick >= *stop_tick_;
        const uint32_t next_tick = next < 0 || stopped ? UINT32_MAX : tracks_[static_cast<std::size_t>(next)].next_tick;
        while (!pending_offs_.empty() && pending_offs_.begin()->first <= next_tick &&
               (!stop_tick_ || pending_offs_.begin()->first < *stop_tick_))
        {
            const auto [off_tick, off] = *pending_offs_.begin();
            pending_offs_.erase(pending_offs_.begin());
            tick_ = std::max(tick_, off_tick);
            HandleNoteOff(off.channel, off.key, off_tick);
        }

        // At the end of a pass of the main loop, the notes that end before it have ended, and the events of the stop
        // tick belong to the next pass. The performer counts the pass there and plays another, or stops. A copy that
        // plays on stops where it was told to.
        if (stopped && !copy_ && PlaysAnotherPass())
        {
            continue;
        }

        if (settings_.player == LibultraPlayer::kAmbrella)
        {
            RunSweeps(next_tick != UINT32_MAX ? next_tick : stop_tick_.value_or(tick_));
        }

        if (next < 0)
        {
            break;
        }

        if (stopped)
        {
            tick_ = *stop_tick_;
            break;
        }

        tick_ = next_tick;
        if (!copy_ && SecondsAt(tick_) > kLongestSeconds)
        {
            performance.truncated = true;
            break;
        }

        TrackState& state = tracks_[static_cast<std::size_t>(next)];
        const std::vector<n64::MidiEvent>& events = song_.tracks[static_cast<std::size_t>(next)];
        HandleEvent(next, events[state.index], tick_);
        PlanLoop();
    }
}

// The times of a zone's envelope on a library whose ramps are straight lines in amplitude. SoundFont's and SFZ's
// attacks are such lines. Their decays and releases are straight lines in decibels. A ramp to nothing stands at -6 dB
// at half of its time, at -20 dB at 9/10 of it and at nothing at its end. No straight line in decibels meets all three.
// The fit follows the top 20 dB. A listener hears that part. A tail remains where the ramp has ended.
void ApplyLineRamps(Zone& zone, double attack, double decay, double attack_ms, double decay_ms, double release_ms)
{
    zone.attack_ms = attack_ms;
    if (decay_ms > 0.0 && decay < attack)
    {
        // A hold, then a line in decibels through the ramp's level at the two places. A fall to nothing stands at -23
        // dB at the ramp's end, and at -40 dB at 1.5 times its time.
        const double end = (decay / attack) * (decay / attack);
        const auto fall_at = [&](double share)
        {
            return -20.0 * std::log10(1.0 - (1.0 - end) * share);
        };

        const double db_per_ramp =
            (fall_at(kLineRampLatePlace) - fall_at(kLineRampEarlyPlace)) / (kLineRampLatePlace - kLineRampEarlyPlace);
        zone.hold_ms = decay_ms * (kLineRampEarlyPlace - fall_at(kLineRampEarlyPlace) / db_per_ramp);
        zone.decay_ms = decay_ms * 100.0 / db_per_ramp;
        zone.sustain_db = decay > 0.0 ? -20.0 * std::log10(end) : 100.0;
    }
    else if (decay_ms > 0.0 && decay > attack)
    {
        // A rise after the attack. The fit lets the attack run on to the decay's end.
        zone.delay_ms = attack_ms;
        zone.attack_ms = decay_ms;
    }

    // A release can't hold, and a line falls to nothing in its time from any level.
    zone.release_ms = release_ms * kLineReleaseTo100Db;
}

// The envelope and the level of a zone. `before_envelope` is the amplitude of the zone without its envelope.
void ApplyEnvelope(Zone& zone, const n64::BankEnvelope& envelope, double before_envelope, bool line_ramps)
{
    const double attack = envelope.attack_volume / 127.0;
    const double decay = envelope.decay_volume / 127.0;
    const double attack_ms = std::max(envelope.attack_time, 0) / 1000.0;
    const double decay_ms = std::max(envelope.decay_time, 0) / 1000.0;
    const double release_ms = std::max(envelope.release_time, 0) / 1000.0;

    // The top is where the envelope's volume is highest. A ramp at once goes straight to its target.
    const double top = decay_ms == 0.0 ? decay : std::max(attack, decay);
    zone.gain = before_envelope * top * top;
    if (line_ramps)
    {
        ApplyLineRamps(zone, attack, decay, attack_ms, decay_ms, release_ms);
        return;
    }

    // How far above the mixer's floor the top is, for a typical note. The ramps run on a straight line in decibels.
    const double nominal = kNominalVelocity / 127.0;
    const double top_db = 20.0 * std::log10(std::max(zone.gain * nominal * nominal, 1e-7)) + kCentreDb;
    const double range_db = std::max(kFloorDb + top_db, 1.0);

    // A rise from the floor in `attack_ms`. SoundFont and SFZ attacks are straight lines in amplitude. A line of the
    // same energy has this length, and the same end.
    if (attack_ms > 0.0)
    {
        const double line_ms = std::min(attack_ms, attack_ms * 3.0 * 10.0 / (range_db * std::numbers::ln10));
        zone.delay_ms = attack_ms - line_ms;
        zone.attack_ms = line_ms;
    }

    double sustain_db = 0.0;
    if (decay_ms > 0.0 && decay < attack)
    {
        // A fall to the decay's volume, or to the floor.
        const double fall_db = decay > 0.0 ? 40.0 * std::log10(attack / decay) : range_db;
        zone.decay_ms = decay_ms * 100.0 / fall_db;
        zone.sustain_db = decay > 0.0 ? fall_db : 100.0;
        sustain_db = fall_db;
    }
    else if (decay_ms > 0.0 && decay > attack)
    {
        // A rise after the attack. The fit lets the attack run on to the decay's end.
        zone.delay_ms = attack_ms;
        zone.attack_ms = decay_ms;
    }

    // A release falls from where the note stands to the floor in the release's time. A fall to the floor has no sustain
    // to start from. A note of that kind usually ends well before its decay's end. The fit therefore takes the release
    // from the top.
    const double release_range_db = decay_ms > 0.0 && decay == 0.0 ? range_db : std::max(range_db - sustain_db, 1.0);
    zone.release_ms = release_ms * 100.0 / release_range_db;
}

// A tremolo's or a vibrato's LFO: the steps of a period and the delay. A wave other than a sine keeps its period and
// its range.
Lfo OscillatorLfo(int type, int rate, int delay)
{
    Lfo lfo;
    const bool sine = type == kTremoloSine || type == kVibratoSine;
    const int steps = std::max((sine ? kSineSteps : kOtherSteps) - rate, 1);
    lfo.hz = 1000.0 / (kOscillatorStepMs * steps);
    lfo.delay_ms = delay * kOscillatorDelayMs;

    return lfo;
}

void ApplyOscillators(Zone& zone, const n64::BankInstrument& instrument)
{
    if (instrument.tremolo_type != 0 && instrument.tremolo_type < kFirstVibrato)
    {
        // The tremolo holds the voice's volume between 127 and 127 less its depth. The mixer squares it.
        const int depth = std::min<int>(instrument.tremolo_depth & 0xFE, 126);
        Lfo lfo = OscillatorLfo(instrument.tremolo_type, instrument.tremolo_rate, instrument.tremolo_delay);
        lfo.depth = 20.0 * std::log10(127.0 / (127.0 - depth));
        zone.tremolo = lfo;
    }

    if (instrument.vibrato_type >= kFirstVibrato)
    {
        Lfo lfo = OscillatorLfo(instrument.vibrato_type, instrument.vibrato_rate, instrument.vibrato_delay);
        lfo.depth = std::pow(kVibratoDepthBase, instrument.vibrato_depth);
        zone.vibrato = lfo;
    }
}

// Ambrella's player moves a note's pitch and volume by CC1 and the bank's depths and rates. It reads neither the
// oscillators' types nor their delays.
void ApplyAmbrellaWheel(Zone& zone, const n64::BankInstrument& instrument)
{
    if (instrument.vibrato_depth != 0 && instrument.vibrato_rate != 0)
    {
        zone.vibrato = Lfo{.hz = instrument.vibrato_rate / kAmbrellaLfoUnits,
                           .depth = instrument.vibrato_depth * kAmbrellaWheelVibrato,
                           .wheel = true};
    }

    if (instrument.tremolo_depth != 0 && instrument.tremolo_rate != 0)
    {
        const double fall = std::min(instrument.tremolo_depth * kAmbrellaWheelTremolo, 0.99);
        zone.tremolo = Lfo{
            .hz = instrument.tremolo_rate / kAmbrellaLfoUnits, .depth = -20.0 * std::log10(1.0 - fall), .wheel = true};
    }
}

// A run of velocities that plays one sound.
struct VelocityRun
{
    friend bool operator==(const VelocityRun&, const VelocityRun&) = default;

    int low = 0;
    int high = 0;
    int32_t sound = n64::kNoObject;
};

// The runs of a key: what the players' lookup finds at each velocity.
std::vector<VelocityRun> RunsOfKey(const n64::BankFile& file, const n64::BankInstrument& instrument, int key)
{
    std::vector<VelocityRun> runs;
    for (int velocity = 1; velocity <= 127; velocity++)
    {
        const int32_t sound = LookupSound(file, instrument, key, velocity);
        if (!runs.empty() && runs.back().sound == sound)
        {
            runs.back().high = velocity;
        }
        else
        {
            runs.push_back({velocity, velocity, sound});
        }
    }

    std::erase_if(runs, [](const VelocityRun& run) { return run.sound == n64::kNoObject; });
    if (!runs.empty() && runs.front().low == 1)
    {
        runs.front().low = 0;
    }

    return runs;
}

} // namespace

LibultraPerformance PerformLibultra(const LibultraSong& song, const n64::BankFile& file, const n64::Bank& bank,
                                    const LibultraSettings& settings, const PerformOptions& options)
{
    Performer performer(song, file, bank, settings, options);
    return performer.Run();
}

std::vector<Zone> LibultraZones(const n64::BankFile& file, int32_t index, const LibultraSettings& settings,
                                std::vector<int32_t>& wave_of_zone)
{
    const n64::BankInstrument& instrument = file.instruments[static_cast<std::size_t>(index)];
    const bool ambrella = settings.player == LibultraPlayer::kAmbrella;
    const double full_voice = ambrella ? kAmbrellaFullVoiceVolume : kFullVoiceVolume;
    const double volume = full_voice * settings.volume / kFullPlayerVolume;

    std::vector<Zone> zones;
    wave_of_zone.clear();

    // Adds the zone of a run over `low_key` to `high_key`.
    const auto add = [&](int low_key, int high_key, const VelocityRun& run)
    {
        const n64::BankSound& sound = file.sounds[static_cast<std::size_t>(run.sound)];
        if (sound.wave == n64::kNoObject || sound.key_map == n64::kNoObject || sound.envelope == n64::kNoObject)
        {
            return;
        }

        const n64::BankKeyMap& map = file.key_maps[static_cast<std::size_t>(sound.key_map)];
        const double sample_volume = settings.player == LibultraPlayer::kHal ? 1.0 : sound.volume / 127.0;
        const double before_envelope = (volume * sample_volume) * (volume * sample_volume);

        Zone zone;
        zone.low_velocity = run.low;
        zone.high_velocity = run.high;
        zone.root_key = map.key_base;
        const int detune = ambrella ? -map.detune : map.detune;
        zone.tune_cents = detune;
        zone.pan = std::clamp((sound.pan - 64) / 64.0, -1.0, 1.0);
        if (ambrella)
        {
            ApplyAmbrellaWheel(zone, instrument);
        }

        const n64::BankEnvelope envelope =
            PlayedEnvelope(file.envelopes[static_cast<std::size_t>(sound.envelope)], settings.player);
        ApplyEnvelope(zone, envelope, before_envelope, settings.line_ramps);
        if (settings.oscillators)
        {
            ApplyOscillators(zone, instrument);
        }

        zone.low_key = low_key;
        zone.high_key = high_key;
        AddUpToResamplerLimit(zones, zone, (map.key_base * 100 + kOctaveCents - detune + 99) / 100, kOctaveCents);
        wave_of_zone.resize(zones.size(), sound.wave);
    };

    int low_key = 0;
    std::vector<VelocityRun> runs = RunsOfKey(file, instrument, 0);
    for (int key = 1; key <= 128; key++)
    {
        std::vector<VelocityRun> next = key <= 127 ? RunsOfKey(file, instrument, key) : std::vector<VelocityRun>();
        if (key == 128 || next != runs)
        {
            for (const VelocityRun& run : runs)
            {
                add(low_key, key - 1, run);
            }

            low_key = key;
            runs = std::move(next);
        }
    }

    return zones;
}

} // namespace ultralib::sequence_export::n64
