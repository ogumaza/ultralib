// SPDX-License-Identifier: MIT

// Unit tests for the main loops of the songs of libultra's sequence players. The tests build small sound files in
// memory. Their expected values follow from the arithmetic of the engines, worked out by hand.

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
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
using ultralib::sequence_export::n64::PerformOptions;
using ultralib::testing::Controller;
using ultralib::testing::EndOfTrack;
using ultralib::testing::EventsOfKind;
using ultralib::testing::kHeldEnvelope;
using ultralib::testing::NoteOn;
using ultralib::testing::Play;
using ultralib::testing::SmallBank;
using xsflib::testing::Check;
using xsflib::testing::Near;

void TestForeverLoop()
{
    const n64::BankFile file = SmallBank(kHeldEnvelope);
    n64::MidiEvent start = {.status = n64::kMidiMeta, .type = n64::kCompressedLoopStart, .payload = {0, 0xFF}};
    n64::MidiEvent end = {.delta = 48,
                          .status = n64::kMidiMeta,
                          .type = n64::kCompressedLoopEnd,
                          .payload = {0xFF, 0xFF, 0, 0, 0, 0},
                          .loop_start = 1};
    const std::vector<n64::MidiEvent> track = {
        NoteOn(0, 60, 100, 24), start, NoteOn(24, 62, 100, 12), end, EndOfTrack(0),
    };

    PerformOptions options;
    options.loops = 2;
    const LibultraPerformance played = Play(track, file, options);
    const ultralib::sequence_export::n64::Performance& performance = played.performance;

    // Each pass runs from tick 0 for 72 ticks. The first note comes before the loop's start in the track: the player
    // plays it once and goes back to the bytes after the start. The first pass has two notes and the later passes
    // one. A MIDI player that loops on the first pass would play the first note again. The loop is the second pass,
    // and two passes follow the first.
    Check(performance.loop.has_value(), "the song loops");
    Check(performance.loop && performance.loop->start == 72 && performance.loop->end == 144, "the loop's ticks");
    Check(performance.loops == 2, "the loop plays twice");
    Check(performance.end_tick == 216, "the song ends after its passes");
    const std::vector<midi::TrackEvent> ons = EventsOfKind(performance, midi::EventKind::kNoteOn);
    Check(ons.size() == 4, "the first note plays once and the loop's note three times");

    // A first note a tick before the loop's start leaves every pass the same. The loop is the first pass, and with
    // one pass the song ends at its end.
    const std::vector<n64::MidiEvent> later = {
        NoteOn(0, 60, 100, 24),  {.delta = 1, .status = n64::kMidiMeta, .type = n64::kCompressedLoopStart},
        NoteOn(23, 62, 100, 12), end,
        EndOfTrack(0),
    };
    const ultralib::sequence_export::n64::Performance once = Play(later, file).performance;
    Check(once.loop && once.loop->start == 1 && once.loop->end == 72, "a loop after the first note: the first pass");
    Check(once.loops == 1 && once.end_tick == 72, "one pass");
}

void TestCountedLoop()
{
    const n64::BankFile file = SmallBank(kHeldEnvelope);
    n64::MidiEvent start = {.status = n64::kMidiMeta, .type = n64::kCompressedLoopStart, .payload = {0, 0xFF}};
    n64::MidiEvent end = {.delta = 48,
                          .status = n64::kMidiMeta,
                          .type = n64::kCompressedLoopEnd,
                          .payload = {2, 2, 0, 0, 0, 0},
                          .loop_start = 0};
    const std::vector<n64::MidiEvent> track = {start, NoteOn(0, 62, 100, 12), end, EndOfTrack(0)};
    const LibultraPerformance played = Play(track, file);

    // A count of 2 repeats twice more: three passes of 48 ticks.
    Check(!played.performance.loop.has_value(), "a counted loop isn't the song's loop");
    Check(EventsOfKind(played.performance, midi::EventKind::kNoteOn).size() == 3, "three passes");
}

// A main loop in which no note starts, once the notes before it have ended, sounds nothing. The song then has no loop.
// It ends with its last sound, and what comes after that stays out.
void TestSilentLoop()
{
    n64::BankEnvelope releasing = kHeldEnvelope;
    releasing.release_time = 100000;
    const n64::BankFile file = SmallBank(releasing);
    n64::MidiEvent start = {.status = n64::kMidiMeta, .type = n64::kCompressedLoopStart, .payload = {0, 0xFF}};
    n64::MidiEvent end = {.delta = 48,
                          .status = n64::kMidiMeta,
                          .type = n64::kCompressedLoopEnd,
                          .payload = {0xFF, 0xFF, 0, 0, 0, 0},
                          .loop_start = 2};
    const std::vector<n64::MidiEvent> track = {
        NoteOn(0, 60, 100, 24), Controller(96, 7, 90), start, Controller(24, 7, 80), end, EndOfTrack(0),
    };

    PerformOptions options;
    options.loops = 2;
    const LibultraPerformance played = Play(track, file, options);
    const ultralib::sequence_export::n64::Performance& performance = played.performance;

    // The note ends at tick 24, and its release of 100 ms ends 9.6 ticks of 10416 microseconds later.
    Check(!performance.loop.has_value() && performance.loops == 0, "a loop that sounds nothing is no loop");
    Check(performance.end_tick == 34, "the song ends at the tick of its last sound");
    Check(Near(performance.seconds, (24 * 10416 + 100000) / 1000000.0, 1e-6), "the song ends with the release");
    const auto after_end = [&performance](const midi::TrackEvent& event)
    {
        return event.tick > performance.end_tick;
    };
    Check(std::ranges::none_of(performance.tracks[0], after_end), "the controllers after the last sound stay out");

    // A note that sounds on into the loop keeps it.
    const std::vector<n64::MidiEvent> held = {
        NoteOn(0, 60, 100, 0), Controller(96, 7, 90), start, Controller(24, 7, 80), end, EndOfTrack(0),
    };
    const LibultraPerformance holding = Play(held, file, options);
    Check(holding.performance.loop.has_value(), "a loop with a note that sounds on is a loop");
}

n64::MidiEvent Program(uint32_t delta, int program)
{
    return {.delta = delta, .status = 0xC0, .data1 = static_cast<uint8_t>(program)};
}

n64::MidiEvent Bend(uint32_t delta, int value)
{
    return {.delta = delta,
            .status = 0xE0,
            .data1 = static_cast<uint8_t>(value & 0x7F),
            .data2 = static_cast<uint8_t>(value >> 7)};
}

n64::MidiEvent Tempo(uint32_t delta, uint32_t microseconds)
{
    return {.delta = delta,
            .status = n64::kMidiMeta,
            .type = n64::kMidiMetaTempo,
            .payload = {static_cast<uint8_t>(microseconds >> 16), static_cast<uint8_t>(microseconds >> 8),
                        static_cast<uint8_t>(microseconds)}};
}

// The ticks from before the first of `events` to the last.
uint32_t TicksOf(const std::vector<n64::MidiEvent>& events)
{
    uint32_t ticks = 0;
    for (const n64::MidiEvent& event : events)
    {
        ticks += event.delta;
    }

    return ticks;
}

// The gap between a looping track's intro and its loop, and between the loop's last event and its end.
constexpr uint32_t kLoopGap = 12;
constexpr uint32_t kLoopTail = 24;

// A track that plays `intro`, then repeats `loop` for ever. The loop starts `gap` ticks after the intro's last event
// and ends `tail` ticks after the loop's last event.
std::vector<n64::MidiEvent> LoopingTrack(const std::vector<n64::MidiEvent>& intro,
                                         const std::vector<n64::MidiEvent>& loop, uint32_t gap = kLoopGap,
                                         uint32_t tail = kLoopTail)
{
    std::vector<n64::MidiEvent> track = intro;
    const auto start = static_cast<int32_t>(track.size());
    track.push_back({.delta = gap, .status = n64::kMidiMeta, .type = n64::kCompressedLoopStart, .payload = {0, 0xFF}});
    track.insert(track.end(), loop.begin(), loop.end());
    track.push_back({.delta = tail,
                     .status = n64::kMidiMeta,
                     .type = n64::kCompressedLoopEnd,
                     .payload = {0xFF, 0xFF, 0, 0, 0, 0},
                     .loop_start = start});
    track.push_back(EndOfTrack(0));

    return track;
}

// A note as a MIDI player plays it: its tick in its pass, its key and velocity, the tempo, and the settings of its
// part. A setting is the program, a controller or the pitch.
struct HeardNote
{
    uint32_t offset = 0;
    uint8_t key = 0;
    uint8_t velocity = 0;
    double bpm = 0.0;
    std::map<std::pair<uint8_t, midi::EventKind>, double> settings;

    friend bool operator==(const HeardNote& a, const HeardNote& b) = default;
};

// What a MIDI player holds as it plays a track: the tempo and each part's settings.
struct HeardState
{
    double bpm = 0.0;
    std::map<std::pair<uint8_t, midi::EventKind>, double> settings;
};

// Plays the events of `performance`'s track `track` from tick `from` up to tick `to` into `state`, with the tempo
// changes between. Adds the notes from tick `notes_from` on to `notes`, at their ticks after `notes_from`.
void Hear(const ultralib::sequence_export::n64::Performance& performance, std::size_t track, uint32_t from, uint32_t to,
          uint32_t notes_from, HeardState& state, std::vector<HeardNote>& notes)
{
    std::size_t tempo = 0;
    const auto tempo_until = [&](uint32_t tick)
    {
        for (; tempo < performance.tempo.size() && performance.tempo[tempo].tick <= tick; tempo++)
        {
            if (performance.tempo[tempo].tick >= from)
            {
                state.bpm = performance.tempo[tempo].bpm;
            }
        }
    };

    for (const midi::TrackEvent& e : performance.tracks[track])
    {
        if (e.tick < from || e.tick >= to || e.kind == midi::EventKind::kNoteOff)
        {
            continue;
        }

        tempo_until(e.tick);
        if (e.kind == midi::EventKind::kNoteOn)
        {
            if (e.tick >= notes_from)
            {
                notes.push_back({e.tick - notes_from, e.key, e.value, state.bpm, state.settings});
            }
        }
        else if (e.kind == midi::EventKind::kPitch)
        {
            state.settings[{e.part, e.kind}] = e.semitones;
        }
        else if (e.kind == midi::EventKind::kPreset)
        {
            state.settings[{e.part, e.kind}] = e.preset;
        }
        else
        {
            state.settings[{e.part, e.kind}] = e.value;
        }
    }

    tempo_until(to - 1);
}

// True if a MIDI player that loops `once` on its markers plays the notes of the first repeat as `more` plays the pass
// after the loop: at the same ticks, with the same settings and tempo. `more` is the same song with one more pass, in
// which the performer has played that pass as the game does.
bool RepeatsAsPlayedOn(const ultralib::sequence_export::n64::Performance& once,
                       const ultralib::sequence_export::n64::Performance& more)
{
    if (!once.loop || !more.loop || once.loop->start != more.loop->start || once.loop->end != more.loop->end)
    {
        return false;
    }

    const uint32_t start = once.loop->start;
    const uint32_t end = once.loop->end;
    bool any_note = false;
    for (std::size_t track = 0; track < once.tracks.size(); track++)
    {
        HeardState looping;
        std::vector<HeardNote> first_pass;
        std::vector<HeardNote> repeat;
        Hear(once, track, 0, end, end, looping, first_pass);
        Hear(once, track, start, end, start, looping, repeat);

        HeardState played;
        std::vector<HeardNote> next_pass;
        Hear(more, track, 0, end + (end - start), end, played, next_pass);
        if (repeat != next_pass)
        {
            return false;
        }

        any_note = any_note || !repeat.empty();
    }

    return any_note;
}

// A song that sets a value in its intro and again one tick into each pass of its loop. A note follows, then another
// value and a second note. In the first pass the value is the same as the last one, and the file has no event for it.
// The game plays every pass from the second on with the value set again.
std::vector<n64::MidiEvent> SetAgainInLoop(n64::MidiEvent first, n64::MidiEvent second)
{
    n64::MidiEvent again = first;
    first.delta = 0;
    again.delta = 1;
    second.delta = 24;

    return LoopingTrack({first, NoteOn(0, 64, 100, 12)},
                        {again, NoteOn(1, 60, 100, 12), second, NoteOn(0, 62, 100, 12)});
}

// The game keeps a channel's settings from pass to pass. A setting that the loop makes again after a pass's first tick
// goes into the file only where it changes. A MIDI player that repeats the first pass keeps the value of the loop's end
// for the notes before it, where the game plays the value set again. The second pass plays as the third, and the loop
// is the second pass.
void TestLoopOnSecondPass()
{
    using ultralib::sequence_export::n64::LibultraPlayer;

    struct Case
    {
        const char* what;
        LibultraSettings settings;
        std::vector<n64::MidiEvent> track;
        uint32_t intro = 0; // the ticks before the loop
        uint32_t pass = 0;  // the ticks of a pass
    };

    // The sweep: a note glides from the channel's last key. The loop's first note sweeps from the intro's note in the
    // first pass, and from the loop's last note in the others.
    const std::vector<n64::MidiEvent> sweep_intro = {Controller(0, 5, 30), Controller(0, 65, 127),
                                                     NoteOn(0, 60, 100, 12)};
    const std::vector<n64::MidiEvent> sweep_loop = {NoteOn(1, 60, 100, 12), NoteOn(24, 64, 100, 12)};
    const LibultraSettings echo = {.effect = n64::AlEffect::kEcho};
    const LibultraSettings no_effect = {.effect = n64::AlEffect::kNone};
    const LibultraSettings ambrella = {.player = LibultraPlayer::kAmbrella};
    const LibultraSettings hal = {.player = LibultraPlayer::kHal};
    const uint32_t set_again_pass = 1 + 1 + 24 + kLoopTail;

    // A send of 1 leaves the dry level and CC11 at 127: only CC91 changes. Without an effect, CC91 stays at 0 and only
    // the dry level changes.
    const std::vector<Case> cases = {
        {"CC7", {}, SetAgainInLoop(Controller(0, 7, 60), Controller(0, 7, 30)), kLoopGap, set_again_pass},
        {"CC10", {}, SetAgainInLoop(Controller(0, 10, 20), Controller(0, 10, 100)), kLoopGap, set_again_pass},
        {"CC91", echo, SetAgainInLoop(Controller(0, 91, 1), Controller(0, 91, 0)), kLoopGap, set_again_pass},
        {"the send's dry level", no_effect, SetAgainInLoop(Controller(0, 91, 100), Controller(0, 91, 1)), kLoopGap,
         set_again_pass},
        {"the bend", {}, SetAgainInLoop(Bend(0, 8192 + 2048), Bend(0, 8192)), kLoopGap, set_again_pass},
        {"Ambrella's CC11", ambrella, SetAgainInLoop(Controller(0, 11, 100), Controller(0, 11, 50)), kLoopGap,
         set_again_pass},
        {"Ambrella's CC1", ambrella, SetAgainInLoop(Controller(0, 1, 90), Controller(0, 1, 30)), kLoopGap,
         set_again_pass},
        {"Ambrella's sweep", ambrella, LoopingTrack(sweep_intro, sweep_loop), kLoopGap,
         TicksOf(sweep_loop) + kLoopTail},
        {"HAL's CC8", hal, SetAgainInLoop(Controller(0, 8, 64), Controller(0, 8, 100)), kLoopGap, set_again_pass},
        {"HAL's CC21", hal, SetAgainInLoop(Controller(0, 21, 100), Controller(0, 21, 60)), kLoopGap, set_again_pass},
    };

    const n64::BankFile file = SmallBank(kHeldEnvelope);
    PerformOptions twice;
    twice.loops = 2;
    PerformOptions three_times;
    three_times.loops = 3;
    for (const Case& c : cases)
    {
        const std::string what = c.what;
        const ultralib::sequence_export::n64::Performance once = Play(c.track, file, {}, c.settings).performance;
        const ultralib::sequence_export::n64::Performance more = Play(c.track, file, twice, c.settings).performance;
        const ultralib::sequence_export::n64::Performance most =
            Play(c.track, file, three_times, c.settings).performance;

        const uint32_t start = c.intro + c.pass;
        Check(once.loop && once.loop->start == start && once.loop->end == start + c.pass,
              what + ": the loop is the second pass");
        Check(once.loops == 1 && once.end_tick == start + c.pass, what + ": the song ends after the loop");
        Check(RepeatsAsPlayedOn(once, more), what + ": a MIDI player repeats the loop as the game plays on");

        // With --loops 2, two passes follow the first.
        Check(more.loop && more.loop->start == start && more.loops == 2 && more.end_tick == start + 2 * c.pass,
              what + ": --loops 2 plays the loop twice");
        Check(RepeatsAsPlayedOn(more, most), what + ": --loops 2 repeats as the game plays on");
    }
}

// A setting that the loop makes again in a pass's first tick, with the value of the loop's start, gets no event in the
// first pass. The game gives it back at each pass's start, and the file sends it again at the loop's start. The loop
// stays on the first pass.
void TestLoopStartRestored()
{
    const n64::BankFile file = SmallBank(kHeldEnvelope);
    const std::vector<n64::MidiEvent> track =
        LoopingTrack({Controller(0, 7, 60), NoteOn(0, 64, 100, 12)},
                     {Controller(0, 7, 60), NoteOn(1, 60, 100, 12), Controller(24, 7, 30), NoteOn(0, 62, 100, 12)});
    PerformOptions twice;
    twice.loops = 2;

    const ultralib::sequence_export::n64::Performance once = Play(track, file).performance;
    const ultralib::sequence_export::n64::Performance more = Play(track, file, twice).performance;

    const uint32_t pass = 1 + 24 + kLoopTail;
    Check(once.loop && once.loop->start == kLoopGap && once.loop->end == kLoopGap + pass, "the loop is the first pass");
    const auto volume_at_start = [](const midi::TrackEvent& event)
    {
        return event.tick == kLoopGap && event.kind == midi::EventKind::kVolume && event.value == 60;
    };
    Check(std::ranges::any_of(once.tracks[0], volume_at_start), "CC7 goes out again at the loop's start");
    Check(RepeatsAsPlayedOn(once, more), "a MIDI player repeats the loop as the game plays on");
}

// True if a note held across the end of `once`'s loop plays with the same settings of track 0 in two runs: in a MIDI
// player that loops on the markers, once the first tick of the repeat has played, and in `more`, once the first tick of
// the game's next pass has played.
bool HoldsAsPlayedOn(const ultralib::sequence_export::n64::Performance& once,
                     const ultralib::sequence_export::n64::Performance& more)
{
    if (!once.loop || !more.loop)
    {
        return false;
    }

    const uint32_t start = once.loop->start;
    const uint32_t end = once.loop->end;
    std::vector<HeardNote> notes;
    HeardState looping;
    Hear(once, 0, 0, end, end, looping, notes);
    Hear(once, 0, start, start + 1, start, looping, notes);

    HeardState played;
    Hear(more, 0, 0, end + 1, end, played, notes);

    return looping.settings == played.settings;
}

// A loop from tick 0 contains each channel's set-up: the instrument's volume and pan. The game sends them once. A MIDI
// player that loops on the markers sends them again at each repeat.
void TestLoopFromTickZero()
{
    const n64::BankFile file = SmallBank(kHeldEnvelope);
    PerformOptions twice;
    twice.loops = 2;
    const auto at_start = [](midi::EventKind kind, int value)
    {
        return [kind, value](const midi::TrackEvent& event)
        {
            return event.tick == 0 && event.kind == kind && event.value == value;
        };
    };

    // The sequence sets the pan and the volume before its note. The note sounds on past the loop's end. The volume
    // comes back to the instrument's 100 before the end. The pan of 90 stays. The file leaves the instrument's pan of
    // 32 out. As in the game, the held note keeps the pan of 90 at a repeat. The volume's event stays.
    const std::vector<n64::MidiEvent> set_later = LoopingTrack(
        {}, {Controller(4, 7, 50), Controller(0, 10, 90), NoteOn(0, 60, 100, 40), Controller(6, 7, 100)}, 0);
    const ultralib::sequence_export::n64::Performance once = Play(set_later, file).performance;
    const ultralib::sequence_export::n64::Performance more = Play(set_later, file, twice).performance;
    Check(once.loop && once.loop->start == 0 && once.loop->end == 4 + 6 + kLoopTail, "the loop is the first pass");
    Check(std::ranges::none_of(once.tracks[0], at_start(midi::EventKind::kPan, 32)),
          "the instrument's pan stays out of the loop");
    Check(std::ranges::any_of(once.tracks[0], at_start(midi::EventKind::kVolume, 100)),
          "a volume that the loop ends with stays");
    Check(HoldsAsPlayedOn(once, more), "a held note keeps the loop end's pan at a repeat");
    Check(RepeatsAsPlayedOn(once, more), "a MIDI player repeats the loop as the game plays on");

    // A sequence that sets the instrument's pan at tick 0 sets it at every pass. The file keeps the pan there.
    const std::vector<n64::MidiEvent> set_at_start = LoopingTrack(
        {}, {Controller(0, 10, 32), Controller(4, 10, 90), NoteOn(0, 60, 100, 40), Controller(6, 7, 100)}, 0);
    const ultralib::sequence_export::n64::Performance sent = Play(set_at_start, file).performance;
    const ultralib::sequence_export::n64::Performance sent_more = Play(set_at_start, file, twice).performance;
    Check(sent.loop && sent.loop->start == 0, "a pan at tick 0: the loop is the first pass");
    Check(std::ranges::any_of(sent.tracks[0], at_start(midi::EventKind::kPan, 32)),
          "a pan that the sequence sets at tick 0 stays");
    Check(HoldsAsPlayedOn(sent, sent_more), "a pan at tick 0: a held note hears it at a repeat");

    // The program at tick 4 sends the instrument's volume of 100 again, and the recorder leaves that value out. The
    // volume then falls to 50 while the note sounds. The file moves the volume of 100 from tick 0 to tick 4. At a
    // repeat in the game and in the file, the held note keeps 50 up to tick 4, and the note at tick 4 plays at 100.
    const std::vector<n64::MidiEvent> program_later =
        LoopingTrack({}, {Program(4, 0), NoteOn(0, 60, 100, 40), Controller(6, 7, 50)}, 0);
    const ultralib::sequence_export::n64::Performance program = Play(program_later, file).performance;
    const ultralib::sequence_export::n64::Performance program_more = Play(program_later, file, twice).performance;
    const auto volume_at_program = [](const midi::TrackEvent& event)
    {
        return event.tick == 4 && event.kind == midi::EventKind::kVolume && event.value == 100;
    };
    Check(program.loop && program.loop->start == 0 && program.loop->end == 4 + 6 + kLoopTail,
          "a program: the loop is the first pass");
    Check(std::ranges::none_of(program.tracks[0], at_start(midi::EventKind::kVolume, 100)),
          "a program: the instrument's volume leaves tick 0");
    Check(std::ranges::any_of(program.tracks[0], volume_at_program), "a program: the volume goes out with it");
    Check(HoldsAsPlayedOn(program, program_more), "a program: a held note keeps the loop end's volume at a repeat");
    Check(RepeatsAsPlayedOn(program, program_more), "a program: a MIDI player repeats the loop as the game plays on");

    // A note before the sequence's pan plays with the instrument's pan. A tick lasts 10416 microseconds, and a pass of
    // 48000 ticks lasts 500 s. The time limit cuts the second pass, and the loop stays on the first pass, untested. The
    // file keeps the instrument's pan for the first note.
    const std::vector<n64::MidiEvent> heard =
        LoopingTrack({},
                     {NoteOn(1, 60, 100, 12), Controller(24, 10, 90), NoteOn(0, 62, 100, 12),
                      NoteOn(48000 - (1 + 24) - kLoopTail, 67, 100, 12)},
                     0);
    const ultralib::sequence_export::n64::Performance cut = Play(heard, file).performance;
    Check(cut.loop && cut.loop->start == 0 && cut.loop->end == 48000, "a long pass: the loop is the first pass");
    Check(std::ranges::any_of(cut.tracks[0], at_start(midi::EventKind::kPan, 32)), "a pan that a note hears stays");
}

// A program goes out with the channel's next note. A program that the loop sets again goes into the file only where it
// changes.
void TestLoopPassOfProgram()
{
    n64::BankFile file = SmallBank(kHeldEnvelope);
    file.instruments.push_back(file.instruments[0]);
    file.banks[0].instruments = {0, 1};
    PerformOptions twice;
    twice.loops = 2;

    // The loop's first note comes 4 ticks into the pass. A MIDI player that repeats the first pass plays it on the
    // program of the loop's end, where the game plays the program set again. The loop is the second pass.
    const std::vector<n64::MidiEvent> later = LoopingTrack(
        {NoteOn(0, 64, 100, 12)}, {Program(0, 0), NoteOn(4, 60, 100, 12), Program(24, 1), NoteOn(0, 62, 100, 12)});
    const ultralib::sequence_export::n64::Performance once = Play(later, file).performance;
    const ultralib::sequence_export::n64::Performance more = Play(later, file, twice).performance;
    const uint32_t pass = 4 + 24 + kLoopTail;
    Check(once.loop && once.loop->start == kLoopGap + pass && once.loop->end == kLoopGap + 2 * pass,
          "a program before a later note: the loop is the second pass");
    Check(RepeatsAsPlayedOn(once, more), "a program before a later note: the repeats play as the game");

    // With the note in the pass's first tick, the file sends the program again at the loop's start. The loop stays on
    // the first pass.
    const std::vector<n64::MidiEvent> first_tick = LoopingTrack(
        {NoteOn(0, 64, 100, 12)}, {Program(0, 0), NoteOn(0, 60, 100, 12), Program(24, 1), NoteOn(0, 62, 100, 12)});
    const ultralib::sequence_export::n64::Performance at_start = Play(first_tick, file).performance;
    const ultralib::sequence_export::n64::Performance at_start_more = Play(first_tick, file, twice).performance;
    const uint32_t short_pass = 24 + kLoopTail;
    Check(at_start.loop && at_start.loop->start == kLoopGap && at_start.loop->end == kLoopGap + short_pass,
          "a program in the first tick: the loop is the first pass");
    const auto preset_at_start = [](const midi::TrackEvent& event)
    {
        return event.tick == kLoopGap && event.kind == midi::EventKind::kPreset;
    };
    Check(std::ranges::any_of(at_start.tracks[0], preset_at_start), "the program goes out again at the loop's start");
    Check(RepeatsAsPlayedOn(at_start, at_start_more), "a program in the first tick: the repeats play as the game");
}

// The tempo, like a setting: a tempo that the loop sets again after the pass's first tick moves the loop to the second
// pass. One set again in the first tick goes out again at the loop's start.
void TestLoopPassOfTempo()
{
    const n64::BankFile file = SmallBank(kHeldEnvelope);
    PerformOptions twice;
    twice.loops = 2;

    const std::vector<n64::MidiEvent> later =
        LoopingTrack({Tempo(0, 400000), NoteOn(0, 64, 100, 12)},
                     {Tempo(1, 400000), NoteOn(1, 60, 100, 12), Tempo(24, 600000), NoteOn(0, 62, 100, 12)});
    const ultralib::sequence_export::n64::Performance once = Play(later, file).performance;
    const ultralib::sequence_export::n64::Performance more = Play(later, file, twice).performance;
    const uint32_t pass = 1 + 1 + 24 + kLoopTail;
    Check(once.loop && once.loop->start == kLoopGap + pass && once.loop->end == kLoopGap + 2 * pass,
          "a tempo after the first tick: the loop is the second pass");
    Check(RepeatsAsPlayedOn(once, more), "a tempo after the first tick: the repeats play as the game");

    const std::vector<n64::MidiEvent> first_tick =
        LoopingTrack({Tempo(0, 400000), NoteOn(0, 64, 100, 12)},
                     {Tempo(0, 400000), NoteOn(1, 60, 100, 12), Tempo(24, 600000), NoteOn(0, 62, 100, 12)});
    const ultralib::sequence_export::n64::Performance at_start = Play(first_tick, file).performance;
    const ultralib::sequence_export::n64::Performance at_start_more = Play(first_tick, file, twice).performance;
    const uint32_t short_pass = 1 + 24 + kLoopTail;
    Check(at_start.loop && at_start.loop->start == kLoopGap && at_start.loop->end == kLoopGap + short_pass,
          "a tempo in the first tick: the loop is the first pass");
    const auto tempo_at_start = [](const midi::Performance::Tempo& tempo)
    {
        return tempo.tick == kLoopGap;
    };
    Check(std::ranges::any_of(at_start.tempo, tempo_at_start), "the tempo goes out again at the loop's start");
    Check(RepeatsAsPlayedOn(at_start, at_start_more), "a tempo in the first tick: the repeats play as the game");
}

// The time limit. The pass choice plays copies of the performer on without it. The performance plays on and the limit
// cuts it.
void TestLoopPassAtTimeLimit()
{
    const n64::BankFile file = SmallBank(kHeldEnvelope);

    // A tick lasts 10416 microseconds. A pass of 33600 ticks lasts 350 s. The third pass ends after 900 s, and the
    // copy still shows that the second pass plays as the third. The loop is the second pass.
    // A last note comes a tail before each pass's end.
    const auto track_of = [](uint32_t pass)
    {
        return LoopingTrack({Controller(0, 7, 60), NoteOn(0, 64, 100, 12)},
                            {Controller(1, 7, 60), NoteOn(1, 60, 100, 12), Controller(24, 7, 30),
                             NoteOn(0, 62, 100, 12), NoteOn(pass - (1 + 1 + 24) - kLoopTail, 67, 100, 12)});
    };
    const ultralib::sequence_export::n64::Performance long_passes = Play(track_of(33600), file).performance;
    Check(long_passes.loop && long_passes.loop->start == kLoopGap + 33600 &&
              long_passes.loop->end == kLoopGap + 2 * 33600,
          "a pass after the time limit shows which pass repeats");
    Check(!long_passes.truncated, "the song ends after the loop");

    // A pass of 48000 ticks lasts 500 s. The second pass plays as the third, and the performance plays on into it. The
    // limit cuts it before its last note, and the loop stays on the first pass.
    const ultralib::sequence_export::n64::Performance cut = Play(track_of(48000), file).performance;
    Check(cut.truncated, "the time limit cuts the second pass");
    Check(cut.loop && cut.loop->start == kLoopGap && cut.loop->end == kLoopGap + 48000 && cut.loops == 1,
          "the loop stays on the first pass");
    Check(cut.end_tick > kLoopGap + 48000 &&
              cut.end_tick * 10416.0 / 1000000.0 > ultralib::sequence_export::n64::kLongestSeconds,
          "the song ends at the time limit");
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("sequence_export_n64_libultra_loops",
                                    []()
                                    {
                                        TestForeverLoop();
                                        TestCountedLoop();
                                        TestSilentLoop();
                                        TestLoopOnSecondPass();
                                        TestLoopStartRestored();
                                        TestLoopFromTickZero();
                                        TestLoopPassOfProgram();
                                        TestLoopPassOfTempo();
                                        TestLoopPassAtTimeLimit();
                                    });
}
