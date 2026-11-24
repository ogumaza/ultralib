// SPDX-License-Identifier: MIT

// Unit tests for the conversion of the songs of libultra's sequence players. The tests build small sound files in
// memory. Their expected values follow from the arithmetic of the engines, worked out by hand.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <numbers>
#include <optional>
#include <string>
#include <vector>

#include "xsflib/testing/check.h"
#include "xsflib/testing/near.h"

#include "sequence_export_n64_testing.h"
#include "ultralib/n64/bank_file.h"
#include "ultralib/n64/midi.h"
#include "ultralib/sequence_export/n64/libultra.h"

namespace
{

namespace n64 = ultralib::n64;
namespace midi = seqlib::midi;
using ultralib::sequence_export::n64::LibultraPerformance;
using ultralib::sequence_export::n64::LibultraSettings;
using ultralib::sequence_export::n64::LibultraSong;
using ultralib::sequence_export::n64::PerformLibultra;
using ultralib::sequence_export::n64::Zone;
using ultralib::testing::Controller;
using ultralib::testing::EndOfTrack;
using ultralib::testing::EventsOfKind;
using ultralib::testing::kHeldEnvelope;
using ultralib::testing::NoteOn;
using ultralib::testing::Play;
using ultralib::testing::SmallBank;
using xsflib::testing::Check;
using xsflib::testing::Near;

// The place in track 0 of the first event of `kind`, or the track's size if it has none.
std::size_t FirstIndexOf(const ultralib::sequence_export::n64::Performance& performance, midi::EventKind kind)
{
    const std::vector<midi::TrackEvent>& events = performance.tracks[0];
    const auto is_kind = [kind](const midi::TrackEvent& event)
    {
        return event.kind == kind;
    };

    return static_cast<std::size_t>(std::ranges::find_if(events, is_kind) - events.begin());
}

void TestNotesAndTempo()
{
    const n64::BankFile file = SmallBank(kHeldEnvelope);
    const std::vector<n64::MidiEvent> track = {
        NoteOn(0, 60, 100, 48),
        NoteOn(96, 62, 127, 0),
        NoteOn(0, 100, 127, 24), // past the key map: plays nothing
        EndOfTrack(48),
    };

    const LibultraPerformance played = Play(track, file);
    const ultralib::sequence_export::n64::Performance& performance = played.performance;

    // The player keeps a whole number of microseconds a tick: 500000 / 48 is 10416.
    Check(performance.tempo.size() == 1, "one tempo");
    Check(Near(performance.tempo[0].bpm, 60000000.0 / (10416.0 * 48.0), 1e-6), "the player's tempo is truncated");

    const std::vector<midi::TrackEvent> ons = EventsOfKind(performance, midi::EventKind::kNoteOn);
    const std::vector<midi::TrackEvent> offs = EventsOfKind(performance, midi::EventKind::kNoteOff);
    Check(ons.size() == 2, "a note that the lookup doesn't find plays nothing");
    Check(offs.size() == 2, "every note ends");
    Check(ons[0].tick == 0 && ons[0].key == 60 && ons[0].value == 100, "the first note");
    Check(offs[0].tick == 48 && offs[0].key == 60, "a compressed note's duration gives its note off");
    Check(ons[1].tick == 96 && ons[1].key == 62, "the second note");
    Check(offs[1].tick == 144 && offs[1].key == 62, "a note without a duration stops at the end");

    // A program goes out with its channel's first note, with the instrument's volume and pan before it.
    Check(played.instruments.size() == 1, "one preset");
    const std::vector<midi::TrackEvent> volumes = EventsOfKind(performance, midi::EventKind::kVolume);
    const std::vector<midi::TrackEvent> pans = EventsOfKind(performance, midi::EventKind::kPan);
    Check(!volumes.empty() && volumes[0].value == 100, "the instrument gives the channel's volume");
    Check(!pans.empty() && pans[0].value == 32, "the instrument gives the channel's pan");

    // A channel starts with no effect send, and CC91 0 goes out before its first note-on, in the note's tick.
    const std::vector<midi::TrackEvent> sends = EventsOfKind(performance, midi::EventKind::kReverb);
    Check(sends.size() == 1 && sends[0].tick == 0 && sends[0].value == 0, "CC91 0 from the start");
    Check(FirstIndexOf(performance, midi::EventKind::kReverb) < FirstIndexOf(performance, midi::EventKind::kNoteOn),
          "CC91 before the first note-on");
}

// The rate divides the time between a song's events. The ticks stay as they are.
void TestLibultraRate()
{
    const n64::BankFile file = SmallBank(kHeldEnvelope);
    const std::vector<n64::MidiEvent> track = {
        NoteOn(0, 60, 100, 48),
        NoteOn(96, 62, 100, 48),
        EndOfTrack(48),
    };

    LibultraSettings settings;
    settings.rate = 0.5;
    const LibultraPerformance plain = Play(track, file);
    const LibultraPerformance slower = Play(track, file, {}, settings);

    // 500000 / 48 is 10416 microseconds a tick, and a rate of 0.5 takes that to 20832.
    Check(slower.performance.tempo.size() == 1, "one tempo at a rate");
    Check(Near(slower.performance.tempo[0].bpm, 60000000.0 / (20832.0 * 48.0), 1e-6), "a rate of 0.5 halves the tempo");
    Check(Near(plain.performance.tempo[0].bpm, 60000000.0 / (10416.0 * 48.0), 1e-6),
          "the default rate keeps the tempo");

    const std::vector<midi::TrackEvent> plain_ons = EventsOfKind(plain.performance, midi::EventKind::kNoteOn);
    const std::vector<midi::TrackEvent> slower_ons = EventsOfKind(slower.performance, midi::EventKind::kNoteOn);
    Check(slower_ons.size() == 2 && plain_ons.size() == 2, "a rate keeps the notes");
    Check(slower_ons[1].tick == plain_ons[1].tick && slower_ons[1].key == plain_ons[1].key, "a rate keeps the ticks");
    Check(Near(slower.performance.seconds, 2.0 * plain.performance.seconds, 1e-6),
          "a rate of 0.5 takes twice the time");
}

void TestUnusedInstrumentsStayOut()
{
    const n64::BankFile file = SmallBank(kHeldEnvelope);
    const LibultraPerformance played = Play({EndOfTrack(48)}, file);
    Check(played.instruments.empty(), "an instrument that plays no note gets no preset");
}

void TestControllers()
{
    const n64::BankFile file = SmallBank(kHeldEnvelope);

    LibultraSettings settings;
    settings.effect = n64::AlEffect::kEcho;

    const std::vector<n64::MidiEvent> track = {
        Controller(0, 91, 64),
        {.status = 0xE0, .data1 = 0x7F, .data2 = 0x7F},
        NoteOn(0, 60, 100, 24),
        EndOfTrack(24),
    };

    const LibultraPerformance played = Play(track, file, {}, settings);
    const ultralib::sequence_export::n64::Performance& performance = played.performance;

    // The dry level at a send of 64 is cos(pi / 2 * 64 / 127), 0.695. CC11 acts squared: 127 * sqrt(0.695) is 106.
    const std::vector<midi::TrackEvent> sends = EventsOfKind(performance, midi::EventKind::kReverb);
    const std::vector<midi::TrackEvent> expressions = EventsOfKind(performance, midi::EventKind::kExpression);
    Check(sends.size() == 1 && sends[0].value == 64, "the send reaches the reverb");
    Check(expressions.size() == 1 && expressions[0].value == 106, "the send takes its share from the dry level");

    // A bend of 8191 of 8192 on a range of 200 cents truncates to 199 cents.
    const std::vector<midi::TrackEvent> bends = EventsOfKind(performance, midi::EventKind::kPitch);
    Check(!bends.empty() && Near(bends[0].semitones, 1.99, 1e-5), "a bend's cents are a whole number");

    // A game without a profile has an unknown effect, and the send goes out as to an effect. A game whose profile
    // gives no effect sends nothing to the reverb: CC91 0 goes before the note.
    settings.effect = std::nullopt;
    const LibultraPerformance unknown = Play(track, file, {}, settings);
    settings.effect = n64::AlEffect::kNone;
    const LibultraPerformance none = Play(track, file, {}, settings);

    const std::vector<midi::TrackEvent> unknown_sends = EventsOfKind(unknown.performance, midi::EventKind::kReverb);
    const std::vector<midi::TrackEvent> no_sends = EventsOfKind(none.performance, midi::EventKind::kReverb);
    Check(unknown_sends.size() == 1 && unknown_sends[0].value == 64, "an unknown effect: the send goes out");
    Check(no_sends.size() == 1 && no_sends[0].value == 0, "no effect: CC91 0");
    Check(FirstIndexOf(none.performance, midi::EventKind::kReverb) <
              FirstIndexOf(none.performance, midi::EventKind::kNoteOn),
          "no effect: CC91 0 before the note-on");
}

const Zone* ZoneAt(const std::vector<Zone>& zones, int key, int velocity)
{
    for (const Zone& zone : zones)
    {
        if (key >= zone.low_key && key <= zone.high_key && velocity >= zone.low_velocity &&
            velocity <= zone.high_velocity)
        {
            return &zone;
        }
    }

    return nullptr;
}

void TestZones()
{
    const n64::BankFile file = SmallBank(kHeldEnvelope);
    std::vector<int32_t> waves;
    LibultraSettings settings;
    const std::vector<Zone> zones = ultralib::sequence_export::n64::LibultraZones(file, 0, settings, waves);
    Check(zones.size() == waves.size(), "each zone has a wave");

    Check(ZoneAt(zones, 47, 100) == nullptr, "a key below the key map has no zone");
    Check(ZoneAt(zones, 85, 100) == nullptr, "a key above the key map has no zone");
    const Zone* low = ZoneAt(zones, 60, 10);
    const Zone* high = ZoneAt(zones, 60, 100);
    Check(low != nullptr && high != nullptr && low != high, "the velocities split between the sounds");
    if (high == nullptr)
    {
        return;
    }

    // With every factor at its top, the player's product is 31507/32768, and the mixer squares it.
    const double full = (31507.0 / 32768.0) * (31507.0 / 32768.0);
    Check(Near(high->gain, full, 1e-6), "the level at the top of every factor");
    Check(high->root_key == 60 && high->follows_key, "the key map's base key");

    // The resampler stops an octave up.
    const Zone* limit = ZoneAt(zones, 72, 100);
    Check(limit != nullptr && !limit->follows_key && limit->tune_cents == 1200.0, "keys past the limit play at it");

    // The player's volume acts squared too.
    settings.volume = 0x3FFF;
    const std::vector<Zone> quieter = ultralib::sequence_export::n64::LibultraZones(file, 0, settings, waves);
    const Zone* half = ZoneAt(quieter, 60, 100);
    const double ratio = 16383.0 / 32767.0;
    Check(half != nullptr && Near(half->gain, full * ratio * ratio, 1e-6), "the player's volume");
}

void TestEnvelope()
{
    const n64::BankEnvelope falling = {
        .attack_time = 100000, .decay_time = 200000, .release_time = 50000, .attack_volume = 127, .decay_volume = 0};
    std::vector<int32_t> waves;
    const std::vector<Zone> zones = ultralib::sequence_export::n64::LibultraZones(SmallBank(falling), 0, {}, waves);
    const Zone* zone = ZoneAt(zones, 60, 100);
    Check(zone != nullptr, "a zone");
    if (zone == nullptr)
    {
        return;
    }

    // At the nominal velocity of 100, the top stands above the floor of 1/32768 by:
    const double gain = (31507.0 / 32768.0) * (31507.0 / 32768.0);
    const double top_db = 20.0 * std::log10(gain * (100.0 / 127.0) * (100.0 / 127.0)) - 3.01;
    const double range_db = 90.31 + top_db;

    // A rise in decibels from the floor has the energy of a straight line in amplitude over T * 30 / (R * ln 10).
    const double line_ms = 100.0 * 30.0 / (range_db * std::numbers::ln10);
    Check(Near(zone->attack_ms, line_ms, 1e-6), "the attack's line");
    Check(Near(zone->delay_ms, 100.0 - line_ms, 1e-6), "the delay before the line ends with the rise");

    // SoundFont counts a decay's time for a fall of 100 dB.
    Check(Near(zone->decay_ms, 200.0 * 100.0 / range_db, 1e-6), "a fall to the floor");
    Check(Near(zone->sustain_db, 100.0, 1e-6), "a fall to the floor sustains at nothing");
    Check(Near(zone->release_ms, 50.0 * 100.0 / range_db, 1e-6), "a release of a falling sound runs from the top");

    const n64::BankEnvelope sustaining = {
        .decay_time = 100000, .release_time = 40000, .attack_volume = 127, .decay_volume = 90};
    const std::vector<Zone> other = ultralib::sequence_export::n64::LibultraZones(SmallBank(sustaining), 0, {}, waves);
    const Zone* second = ZoneAt(other, 60, 100);
    const double fall_db = 40.0 * std::log10(127.0 / 90.0);
    Check(second != nullptr && Near(second->sustain_db, fall_db, 1e-6), "the mixer squares the decay's volume");
    Check(second != nullptr && Near(second->release_ms, 40.0 * 100.0 / (range_db - fall_db), 1e-6),
          "a release runs from the sustain to the floor");
}

// A library's ramps as straight lines in amplitude. The game's output on the exact RSP follows such lines. The fit
// meets them at half of a ramp's time and at 9/10 of it.
void TestLineRampEnvelope()
{
    LibultraSettings settings;
    settings.line_ramps = true;

    // The level in decibels of a zone's decay, `ms` after its attack.
    const auto level_at = [](const Zone& zone, double ms)
    {
        return -std::max(ms - zone.hold_ms, 0.0) * 100.0 / zone.decay_ms;
    };

    std::vector<int32_t> waves;
    const n64::BankEnvelope falling = {
        .attack_time = 100000, .decay_time = 200000, .release_time = 50000, .attack_volume = 127, .decay_volume = 0};
    const std::vector<Zone> zones =
        ultralib::sequence_export::n64::LibultraZones(SmallBank(falling), 0, settings, waves);
    const Zone* zone = ZoneAt(zones, 60, 100);
    Check(zone != nullptr, "the zone of a falling sound");
    if (zone == nullptr)
    {
        return;
    }

    Check(Near(zone->attack_ms, 100.0, 1e-6) && Near(zone->delay_ms, 0.0, 1e-6), "an attack is the same line");
    Check(Near(level_at(*zone, 100.0), 20.0 * std::log10(0.5), 1e-9), "a fall to nothing at half of its time");
    Check(Near(level_at(*zone, 180.0), 20.0 * std::log10(0.1), 1e-9), "a fall to nothing at 9/10 of its time");
    Check(Near(zone->sustain_db, 100.0, 1e-6), "a fall to nothing sustains at nothing");
    Check(Near(zone->release_ms, 50.0 * 5.0, 1e-6), "a release falls 20 dB in its time");

    // The level doesn't change the times: a line falls to nothing in its time from any level.
    settings.volume = 0x3FFF;
    const std::vector<Zone> quieter =
        ultralib::sequence_export::n64::LibultraZones(SmallBank(falling), 0, settings, waves);
    const Zone* quiet = ZoneAt(quieter, 60, 100);
    Check(quiet != nullptr && Near(quiet->decay_ms, zone->decay_ms, 1e-6) &&
              Near(quiet->release_ms, zone->release_ms, 1e-6),
          "the times don't depend on the level");
    settings.volume = 0x7FFF;

    const n64::BankEnvelope sustaining = {
        .decay_time = 100000, .release_time = 40000, .attack_volume = 127, .decay_volume = 30};
    const std::vector<Zone> other =
        ultralib::sequence_export::n64::LibultraZones(SmallBank(sustaining), 0, settings, waves);
    const Zone* second = ZoneAt(other, 60, 100);
    Check(second != nullptr, "the zone of a sustaining sound");
    if (second == nullptr)
    {
        return;
    }

    // The mixer squares the volume, and the line runs from 1 to the square of 30/127.
    const double end = (30.0 / 127.0) * (30.0 / 127.0);
    Check(Near(second->sustain_db, -20.0 * std::log10(end), 1e-9), "a fall to a level ends at the level");
    Check(Near(level_at(*second, 50.0), 20.0 * std::log10(1.0 - (1.0 - end) * 0.5), 1e-9), "a fall to a level at half");
    Check(Near(level_at(*second, 90.0), 20.0 * std::log10(1.0 - (1.0 - end) * 0.9), 1e-9), "a fall to a level at 9/10");
    Check(Near(second->release_ms, 40.0 * 5.0, 1e-6), "a release from a sustain takes the same time");

    // A library that multiplies keeps its fit.
    settings.line_ramps = false;
    const std::vector<Zone> standard =
        ultralib::sequence_export::n64::LibultraZones(SmallBank(falling), 0, settings, waves);
    const Zone* multiplying = ZoneAt(standard, 60, 100);
    Check(multiplying != nullptr && Near(multiplying->hold_ms, 0.0, 1e-6) && multiplying->delay_ms > 0.0,
          "the fit of ramps in decibels holds nothing");
}

// The report counts the notes whose decay or release the fit of straight ramps approximates.
void TestLineRampApproximation()
{
    const std::string fitted = "straight-line decays and releases fitted to their top 20 dB";
    const n64::BankEnvelope falling = {
        .attack_time = 100000, .decay_time = 200000, .release_time = 50000, .attack_volume = 127, .decay_volume = 0};
    const n64::BankEnvelope releasing = {.release_time = 50000, .attack_volume = 127, .decay_volume = 127};
    const std::vector<n64::MidiEvent> track = {NoteOn(0, 60, 100, 24), NoteOn(48, 62, 100, 24), EndOfTrack(48)};
    LibultraSettings settings;
    settings.line_ramps = true;

    const LibultraPerformance falls = Play(track, SmallBank(falling), {}, settings);
    const LibultraPerformance releases = Play(track, SmallBank(releasing), {}, settings);
    const LibultraPerformance holds = Play(track, SmallBank(kHeldEnvelope), {}, settings);
    settings.line_ramps = false;
    const LibultraPerformance multiplies = Play(track, SmallBank(falling), {}, settings);

    const std::map<std::string, uint32_t>& approximations = falls.performance.approximations;
    Check(approximations.contains(fitted) && approximations.at(fitted) == 2, "each note on a decay counts");
    Check(releases.performance.approximations.contains(fitted), "a release alone counts");
    Check(holds.performance.approximations.empty(), "an envelope that holds needs no fit");
    Check(multiplies.performance.approximations.empty(), "a library that multiplies has no straight ramps");
}

void TestOscillators()
{
    n64::BankFile file = SmallBank(kHeldEnvelope);
    n64::BankInstrument& instrument = file.instruments[0];
    instrument.vibrato_type = 0x80;
    instrument.vibrato_rate = 3;
    instrument.vibrato_depth = 100;
    instrument.vibrato_delay = 10;
    instrument.tremolo_type = 1;
    instrument.tremolo_rate = 59;
    instrument.tremolo_depth = 64;

    std::vector<int32_t> waves;
    LibultraSettings settings;
    const std::vector<Zone> without = ultralib::sequence_export::n64::LibultraZones(file, 0, settings, waves);
    const Zone* plain = ZoneAt(without, 60, 100);
    Check(plain != nullptr && !plain->vibrato && !plain->tremolo, "a player without oscillators ignores them");

    settings.oscillators = true;
    const std::vector<Zone> with = ultralib::sequence_export::n64::LibultraZones(file, 0, settings, waves);
    const Zone* zone = ZoneAt(with, 60, 100);
    Check(zone != nullptr && zone->vibrato && zone->tremolo, "a player with oscillators applies them");
    if (zone == nullptr || !zone->vibrato || !zone->tremolo)
    {
        return;
    }

    // A sine has 259 - rate steps of 16 ms, and a delay's unit is 16.384 ms.
    Check(Near(zone->vibrato->hz, 1000.0 / (16.0 * 256.0), 1e-6), "the vibrato's rate");
    Check(Near(zone->vibrato->delay_ms, 163.84, 1e-6), "the vibrato's delay");
    Check(Near(zone->vibrato->depth, std::pow(1.03099303, 100.0), 1e-6), "the vibrato's depth");
    Check(Near(zone->tremolo->hz, 1000.0 / (16.0 * 200.0), 1e-6), "the tremolo's rate");
    Check(Near(zone->tremolo->depth, 20.0 * std::log10(127.0 / 63.0), 1e-6), "the tremolo's depth");
}

void TestAmbrellaLoop()
{
    // Controllers 102 and 103 mark the loop. A note that's on at the loop's end ends there.
    const LibultraSettings settings = {.player = ultralib::sequence_export::n64::LibultraPlayer::kAmbrella};
    const n64::BankFile file = SmallBank(kHeldEnvelope);
    const std::vector<n64::MidiEvent> track = {
        {.delta = 0, .status = 0xC0, .data1 = 0},
        Controller(10, 102, 0),
        {.delta = 20, .status = 0x90, .data1 = 60, .data2 = 100},
        Controller(100, 103, 0),
        EndOfTrack(0),
    };

    const LibultraPerformance played = Play(track, file, {}, settings);
    const ultralib::sequence_export::n64::Performance& performance = played.performance;
    Check(performance.loop.has_value() && performance.loop->start == 10 && performance.loop->end == 130, "the loop");
    Check(performance.end_tick == 130, "the end of the pass");
    const std::vector<midi::TrackEvent> offs = EventsOfKind(performance, midi::EventKind::kNoteOff);
    Check(offs.size() == 1 && offs[0].tick == 130, "the note ends with the loop");
}

void TestAmbrellaControllers()
{
    // A channel starts at a volume of 100. CC11 holds its value at a dry mix. An effect's send of 127 leaves no dry
    // level.
    const LibultraSettings settings = {.player = ultralib::sequence_export::n64::LibultraPlayer::kAmbrella,
                                       .effect = n64::AlEffect::kCustom};
    const n64::BankFile file = SmallBank(kHeldEnvelope);
    const std::vector<n64::MidiEvent> track = {
        {.delta = 0, .status = 0xC0, .data1 = 0},
        Controller(0, 11, 64),
        Controller(0, 1, 90),
        NoteOn(0, 60, 100, 48),
        Controller(48, 91, 127),
        EndOfTrack(0),
    };

    const ultralib::sequence_export::n64::Performance performance = Play(track, file, {}, settings).performance;
    Check(EventsOfKind(performance, midi::EventKind::kVolume).at(0).value == 100, "the first volume");
    const std::vector<midi::TrackEvent> expressions = EventsOfKind(performance, midi::EventKind::kExpression);
    Check(expressions.at(0).value == 64, "CC11 passes through");
    Check(expressions.back().value == 0, "the send takes the dry level");
    Check(EventsOfKind(performance, midi::EventKind::kModulation).at(0).value == 90, "CC1 passes through");
}

void TestAmbrellaSweep()
{
    // CC5 of 30 moves 768 / 31 = 24 cents in each update of 5.75 ms. At 480 ticks to a quarter note of 500000 us, an
    // update is 5.52 ticks. A step of two updates is 11 ticks in the file. A channel's last key starts at 64. The first
    // note, 60, therefore starts 400 cents up. A note 2 semitones above the last starts 200 cents down.
    const LibultraSettings settings = {.player = ultralib::sequence_export::n64::LibultraPlayer::kAmbrella};
    const n64::BankFile file = SmallBank(kHeldEnvelope);
    LibultraSong song;
    song.division = 480;
    song.tracks.push_back({
        {.delta = 0, .status = 0xC0, .data1 = 0},
        Controller(0, 5, 30),
        Controller(0, 65, 127),
        NoteOn(0, 60, 100, 200),
        NoteOn(200, 62, 100, 200),
        EndOfTrack(200),
    });

    const ultralib::sequence_export::n64::Performance performance =
        PerformLibultra(song, file, file.banks[0], settings, {}).performance;
    const std::vector<midi::TrackEvent> pitches = EventsOfKind(performance, midi::EventKind::kPitch);
    Check(!pitches.empty() && pitches[0].tick == 0 && Near(pitches[0].semitones, 4.0, 1e-5),
          "the first note sweeps from key 64");
    Check(!pitches.empty() && pitches.back().tick > 200, "the sweep runs on after the note starts");
}

void TestAmbrellaNoteOff()
{
    // Two notes of key 60 overlap on a channel. The first note-off, at tick 100, releases both voices.
    const LibultraSettings settings = {.player = ultralib::sequence_export::n64::LibultraPlayer::kAmbrella};
    n64::BankFile file = SmallBank(kHeldEnvelope);
    file.envelopes[0].release_time = 100000;
    const std::vector<n64::MidiEvent> track = {
        {.delta = 0, .status = 0xC0, .data1 = 0},
        NoteOn(0, 60, 100, 100),
        NoteOn(20, 60, 100, 200),
        EndOfTrack(300),
    };

    const std::vector<midi::TrackEvent> offs =
        EventsOfKind(Play(track, file, {}, settings).performance, midi::EventKind::kNoteOff);
    Check(offs.size() == 2 && offs[0].tick == 100 && offs[1].tick == 100, "a note-off releases every voice of its key");

    // A sound without a release ends the newest voice at once, and the walk stops there. The older voice ends at the
    // next note-off of its key.
    file.envelopes[0].release_time = 0;
    const std::vector<midi::TrackEvent> sudden =
        EventsOfKind(Play(track, file, {}, settings).performance, midi::EventKind::kNoteOff);
    Check(sudden.size() == 2 && sudden[0].tick == 100 && sudden[1].tick == 220, "without a release, the newest ends");
}

void TestHalScales()
{
    // HAL's player: the player's scale starts at 120 and the channel's at 127, out of 128. CC21 moves every channel's
    // level, and CC8 one channel's.
    const LibultraSettings settings = {.player = ultralib::sequence_export::n64::LibultraPlayer::kHal};
    const n64::BankFile file = SmallBank(kHeldEnvelope);
    const std::vector<n64::MidiEvent> track = {
        {.delta = 0, .status = 0xC0, .data1 = 0},
        Controller(10, 21, 100),
        Controller(10, 8, 64),
        EndOfTrack(0),
    };

    const LibultraPerformance played = Play(track, file, {}, settings);
    const std::vector<midi::TrackEvent> sent = EventsOfKind(played.performance, midi::EventKind::kExpression);

    // 120 * 127 / 128 is 119. 100 * 127 / 128 is 99. 100 * 64 / 128 is 50.
    Check(sent.size() == 3 && sent[0].tick == 0 && sent[0].value == 119, "the start");
    Check(sent[1].tick == 10 && sent[1].value == 99, "the player's scale");
    Check(sent[2].tick == 20 && sent[2].value == 50, "the channel's scale");

    // The sound's volume has no part in the level.
    n64::BankFile quiet = SmallBank(kHeldEnvelope);
    for (n64::BankSound& sound : quiet.sounds)
    {
        sound.volume = 64;
    }

    std::vector<int32_t> waves;
    const std::vector<Zone> full = ultralib::sequence_export::n64::LibultraZones(file, 0, settings, waves);
    waves.clear();
    const std::vector<Zone> same = ultralib::sequence_export::n64::LibultraZones(quiet, 0, settings, waves);
    Check(Near(full[0].gain, same[0].gain, 1e-6), "no sound's volume");
}

void TestAmbrellaZones()
{
    // The player ignores the attack and any sustain short of nothing. The voice's top is 29769 / 32768, squared. CC1
    // sets the bank's vibrato, at 127 * 127 / (128 * 128) of its depth and a rate over 16 in hertz. The detune's sign
    // turns.
    n64::BankEnvelope envelope = {
        .attack_time = 50000, .decay_time = 100000, .release_time = 10000, .attack_volume = 64, .decay_volume = 32};
    n64::BankFile file = SmallBank(envelope);
    file.instruments[0].vibrato_type = 128;
    file.instruments[0].vibrato_rate = 80;
    file.instruments[0].vibrato_depth = 20;
    file.key_maps[0].detune = 10;

    const LibultraSettings settings = {.player = ultralib::sequence_export::n64::LibultraPlayer::kAmbrella};
    std::vector<int32_t> waves;
    const std::vector<Zone> zones = ultralib::sequence_export::n64::LibultraZones(file, 0, settings, waves);
    const Zone& zone = zones.at(0);
    const double top = 29769.0 / 32768.0;
    Check(Near(zone.gain, top * top, 1e-9), "the voice's top");
    Check(zone.attack_ms == 0.0 && zone.delay_ms == 0.0 && zone.decay_ms == 0.0, "no attack and no decay");
    Check(Near(zone.tune_cents, -10.0, 1e-6), "the detune's sign");
    Check(zone.vibrato.has_value() && zone.vibrato->wheel, "CC1 moves the vibrato");
    Check(Near(zone.vibrato->hz, 5.0, 1e-6), "the vibrato's rate");
    Check(Near(zone.vibrato->depth, 20.0 * 127.0 * 127.0 / (128.0 * 128.0), 1e-6), "the vibrato's depth");

    // A fall to nothing stays.
    file.envelopes[0].decay_volume = 0;
    const std::vector<Zone> falling = ultralib::sequence_export::n64::LibultraZones(file, 0, settings, waves);
    Check(falling.at(0).decay_ms > 0.0, "a fall to nothing");
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("sequence_export_n64_libultra",
                                    []()
                                    {
                                        TestNotesAndTempo();
                                        TestLibultraRate();
                                        TestUnusedInstrumentsStayOut();
                                        TestControllers();
                                        TestZones();
                                        TestEnvelope();
                                        TestLineRampEnvelope();
                                        TestLineRampApproximation();
                                        TestOscillators();
                                        TestAmbrellaLoop();
                                        TestAmbrellaControllers();
                                        TestAmbrellaSweep();
                                        TestAmbrellaNoteOff();
                                        TestAmbrellaZones();
                                        TestHalScales();
                                    });
}
