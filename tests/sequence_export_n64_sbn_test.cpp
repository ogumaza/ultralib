// SPDX-License-Identifier: MIT

// Unit tests for the conversion of the songs of the SBN engine. The tests build small sound files in memory. Their
// expected values follow from the arithmetic of the engines, worked out by hand.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <span>
#include <vector>

#include "xsflib/testing/check.h"
#include "xsflib/testing/near.h"

#include "sequence_export_n64_testing.h"
#include "ultralib/sequence_export/n64/games.h"
#include "ultralib/sequence_export/n64/sbn.h"

namespace
{

namespace n64 = ultralib::n64;
namespace midi = seqlib::midi;
using ultralib::sequence_export::n64::Zone;
using ultralib::testing::AllEventsOfKind;
using xsflib::testing::Check;
using xsflib::testing::Near;

// A track of a BGM file: its commands, and its word's flags. A word's polyphony is in bits 13 to 15.
struct BgmTestTrack
{
    std::vector<uint8_t> commands;
    uint32_t flags = 1U << 13;
};

constexpr uint32_t kBgmTwoVoices = 5U << 13;
constexpr uint32_t kBgmPlayPhrase = 1U << 28;
constexpr uint32_t kBgmStartLoop = 3U << 28;
constexpr uint32_t kBgmEndLoop = 5U << 28;

// Builds a BGM file of one composition. A word of `composition` with kBgmPlayPhrase names a phrase by its index, and
// any other word goes in as it stands. `tail` comes after the commands, and `tail_at` gets its place.
std::vector<uint8_t> BgmTestFile(const std::vector<std::vector<BgmTestTrack>>& phrases,
                                 const std::vector<uint32_t>& composition, const std::vector<uint8_t>& tail = {},
                                 std::size_t* tail_at = nullptr)
{
    constexpr std::size_t kHeaderSize = 0x24;
    constexpr std::size_t kPhraseSize = 0x40;
    std::vector<uint8_t> bytes(kHeaderSize, 0);
    std::copy_n("BGM ", 4, bytes.begin());
    std::copy_n("TEST", 4, bytes.begin() + 8);
    bytes[0x14] = 0;
    bytes[0x15] = kHeaderSize / 4;

    const auto add_word = [&bytes](uint32_t word)
    {
        for (int shift = 24; shift >= 0; shift -= 8)
        {
            bytes.push_back(static_cast<uint8_t>(word >> shift));
        }
    };

    const std::size_t phrases_at = kHeaderSize + (composition.size() + 1) * 4;
    for (const uint32_t word : composition)
    {
        const bool plays = (word & kBgmPlayPhrase) != 0 && (word >> 28) == 1;
        add_word(plays ? kBgmPlayPhrase |
                             static_cast<uint32_t>((phrases_at + (word & 0xFFFF) * kPhraseSize - kHeaderSize) / 4)
                       : word);
    }

    add_word(0);

    std::vector<uint8_t> commands;
    std::size_t commands_at = phrases_at + phrases.size() * kPhraseSize;
    for (std::size_t p = 0; p < phrases.size(); p++)
    {
        const std::size_t phrase_at = phrases_at + p * kPhraseSize;
        for (std::size_t t = 0; t < kPhraseSize / 4; t++)
        {
            if (t >= phrases[p].size())
            {
                add_word(0);
                continue;
            }

            add_word(static_cast<uint32_t>(commands_at + commands.size() - phrase_at) << 16 | phrases[p][t].flags);
            commands.insert(commands.end(), phrases[p][t].commands.begin(), phrases[p][t].commands.end());
        }
    }

    bytes.insert(bytes.end(), commands.begin(), commands.end());
    if (tail_at != nullptr)
    {
        *tail_at = bytes.size();
    }

    bytes.insert(bytes.end(), tail.begin(), tail.end());
    bytes.resize((bytes.size() + 3) / 4 * 4, 0);
    const auto size = static_cast<uint32_t>(bytes.size());
    for (std::size_t i = 0; i < 4; i++)
    {
        bytes[4 + i] = static_cast<uint8_t>(size >> (24 - 8 * i));
    }

    return bytes;
}

ultralib::sequence_export::n64::SbnPerformance PlayBgm(const std::vector<uint8_t>& bytes,
                                                       ultralib::sequence_export::n64::SbnSettings settings = {},
                                                       std::span<const n64::BgmInstrument> prg = {})
{
    const n64::BgmFile file = n64::BgmFile::Parse(bytes);
    const ultralib::sequence_export::n64::SbnFacts facts = [](uint16_t)
    {
        return ultralib::sequence_export::n64::SbnInstrumentFacts{.exists = true,
                                                                  .key_base = 6000,
                                                                  .sample_rate = 32000,
                                                                  .samples = 0.0,
                                                                  .loops = true,
                                                                  .release_seconds = 0.1};
    };

    return ultralib::sequence_export::n64::PerformSbn(file, {}, prg, facts, settings, {});
}

// Sets an instrument. Each note holds a key's command, a velocity and a length.
constexpr std::array<uint8_t, 3> kBgmInstrument = {0xE8, 0x30, 0x00};

std::vector<uint8_t> BgmCommands(std::initializer_list<uint8_t> commands)
{
    std::vector<uint8_t> out(kBgmInstrument.begin(), kBgmInstrument.end());
    out.insert(out.end(), commands);

    return out;
}

void TestSbnClock()
{
    // A tempo of 200 at 48 ticks a beat runs 160 ticks a second at the game's nominal rate. The game's interval of
    // 10434782 / 48 drops its fraction, and that adds a few parts in a million.
    const ultralib::sequence_export::n64::SbnPerformance played =
        PlayBgm(BgmTestFile({{{BgmCommands({0xE0, 0x00, 200, 0xBC, 100, 10, 20, 0x00})}}}, {kBgmPlayPhrase}));
    const ultralib::sequence_export::n64::Performance& performance = played.performance;
    Check(performance.timebase == 48, "a tick is a 48th of a beat");
    Check(Near(performance.tempo.back().bpm, 200.0 * 32006.0 / 32000.0, 1e-3), "the tempo at the output's real rate");
    Check(performance.end_tick == 20, "a phrase ends with its track");
    Check(Near(performance.seconds, 20.0 / 160.0, 0.02), "the seconds of the ticks");

    const std::vector<midi::TrackEvent> ons = AllEventsOfKind(performance, midi::EventKind::kNoteOn);
    const std::vector<midi::TrackEvent> offs = AllEventsOfKind(performance, midi::EventKind::kNoteOff);
    Check(ons.size() == 1 && ons[0].key == 60 && ons[0].value == 100, "a note's key and velocity");
    Check(offs.size() == 1 && offs[0].tick == 10, "a note's length");
}

void TestSbnShortNote()
{
    // A note of 1 tick starts no voice, but a track of one voice has cut the note before.
    const ultralib::sequence_export::n64::SbnPerformance played =
        PlayBgm(BgmTestFile({{{BgmCommands({0xBC, 100, 50, 5, 0xC0, 100, 1, 20, 0x00})}}}, {kBgmPlayPhrase}));
    const std::vector<midi::TrackEvent> ons = AllEventsOfKind(played.performance, midi::EventKind::kNoteOn);
    const std::vector<midi::TrackEvent> offs = AllEventsOfKind(played.performance, midi::EventKind::kNoteOff);
    Check(ons.size() == 1 && ons[0].key == 60, "a note of 1 tick doesn't sound");
    Check(offs.size() == 1 && offs[0].tick == 5, "a note of 1 tick cuts the one before");
}

void TestSbnVoices()
{
    {
        // A track of one voice cuts its note for the next.
        const ultralib::sequence_export::n64::SbnPerformance played =
            PlayBgm(BgmTestFile({{{BgmCommands({0xBC, 100, 50, 5, 0xC0, 100, 50, 60, 0x00})}}}, {kBgmPlayPhrase}));
        const std::vector<midi::TrackEvent> offs = AllEventsOfKind(played.performance, midi::EventKind::kNoteOff);
        Check(offs.at(0).tick == 5 && offs.at(0).key == 60, "one voice cuts its note");
        Check(played.presets.at(0).cuts, "one voice's preset cuts");
    }

    {
        // With two voices held, the note with the least time left gives up its voice.
        const ultralib::sequence_export::n64::SbnPerformance played = PlayBgm(
            BgmTestFile({{{BgmCommands({0xBC, 100, 100, 0xC0, 100, 20, 5, 0xC4, 100, 50, 120, 0x00}), kBgmTwoVoices}}},
                        {kBgmPlayPhrase}));
        const std::vector<midi::TrackEvent> offs = AllEventsOfKind(played.performance, midi::EventKind::kNoteOff);
        Check(offs.at(0).tick == 5 && offs.at(0).key == 64, "the shortest note gives up its voice");
        Check(offs.at(1).tick == 55, "the new note's length");
        Check(offs.at(2).tick == 100 && offs.at(2).key == 60, "the longest note keeps its voice");
        Check(!played.presets.at(0).cuts, "two voices' preset doesn't cut");
    }

    {
        // A voice stays busy through its release. A note that has ended gives up its voice first.
        const ultralib::sequence_export::n64::SbnPerformance played = PlayBgm(
            BgmTestFile({{{BgmCommands({0xBC, 100, 5, 0xC0, 100, 100, 10, 0xC4, 100, 50, 120, 0x00}), kBgmTwoVoices}}},
                        {kBgmPlayPhrase}));
        const std::vector<midi::TrackEvent> offs = AllEventsOfKind(played.performance, midi::EventKind::kNoteOff);
        Check(offs.at(0).key == 60 && offs.at(0).tick == 5, "a note that reaches its end");
        Check(offs.at(1).key == 68 && offs.at(1).tick == 60, "the third note takes the released voice");
        Check(offs.at(2).key == 64 && offs.at(2).tick == 100, "the held note keeps its voice");
    }
}

void TestSbnComposition()
{
    const std::vector<std::vector<BgmTestTrack>> phrases = {{{BgmCommands({0xBC, 100, 5, 10, 0x00})}},
                                                            {{BgmCommands({0xC0, 100, 5, 20, 0x00})}}};
    {
        // An endless loop's end ends the performance, and its label gives the start.
        const ultralib::sequence_export::n64::SbnPerformance played =
            PlayBgm(BgmTestFile(phrases, {kBgmPlayPhrase, kBgmStartLoop | 1, kBgmPlayPhrase | 1, kBgmEndLoop | 1}));
        const ultralib::sequence_export::n64::Performance& performance = played.performance;
        Check(performance.loop && performance.loop->start == 10 && performance.loop->end == 30, "an endless loop");
        Check(performance.end_tick == 30, "the end of an endless loop");
    }

    {
        // A loop with a count of 2 plays three passes.
        const ultralib::sequence_export::n64::SbnPerformance played = PlayBgm(
            BgmTestFile(phrases, {kBgmPlayPhrase, kBgmStartLoop | 1, kBgmPlayPhrase | 1, kBgmEndLoop | (2U << 5) | 1}));
        const ultralib::sequence_export::n64::Performance& performance = played.performance;
        Check(!performance.loop && performance.end_tick == 70, "a counted loop");
        Check(AllEventsOfKind(performance, midi::EventKind::kNoteOn).size() == 4, "a counted loop's notes");
    }

    {
        // The end of any track ends the phrase for all of them.
        const ultralib::sequence_export::n64::SbnPerformance played = PlayBgm(BgmTestFile(
            {{{BgmCommands({0xBC, 100, 50, 20, 0x00})}, {BgmCommands({0xC0, 100, 50, 60, 0xC4, 100, 5, 5, 0x00})}},
             {{BgmCommands({0xC8, 100, 5, 5, 0x00})}}},
            {kBgmPlayPhrase, kBgmPlayPhrase | 1}));
        const std::vector<midi::TrackEvent> ons = AllEventsOfKind(played.performance, midi::EventKind::kNoteOn);
        Check(played.performance.end_tick == 25, "a phrase ends with its shortest track");
        Check(std::ranges::none_of(ons, [](const midi::TrackEvent& on) { return on.key == 68; }),
              "a later note stays out");
        Check(std::ranges::any_of(ons, [](const midi::TrackEvent& on) { return on.key == 72 && on.tick == 20; }),
              "the next phrase starts in the tick of the end");
    }
}

void TestSbnMix()
{
    // A branch reads its table by the song's mix. Its tail holds a place and a drum flag for each value.
    const auto file_of = [](std::size_t table)
    {
        const auto hi = static_cast<uint8_t>(table >> 8);
        const auto lo = static_cast<uint8_t>(table);
        return BgmCommands({0xFC, hi, lo, 2, 0, 1, 0x00});
    };

    const std::vector<uint8_t> branch_a = {0xBC, 100, 5, 10, 0x00};
    const std::vector<uint8_t> branch_b = {0xC8, 100, 5, 10, 0x00};
    const std::size_t tail_size = 6;
    std::size_t tail_at = 0;
    std::vector<uint8_t> tail(tail_size + branch_a.size() + branch_b.size(), 0);
    BgmTestFile({{{file_of(0)}}}, {kBgmPlayPhrase}, tail, &tail_at);

    const std::size_t a_at = tail_at + tail_size;
    const std::size_t b_at = a_at + branch_a.size();
    std::vector<uint8_t> filled = {static_cast<uint8_t>(a_at >> 8), static_cast<uint8_t>(a_at), 0,
                                   static_cast<uint8_t>(b_at >> 8), static_cast<uint8_t>(b_at), 0};
    filled.insert(filled.end(), branch_a.begin(), branch_a.end());
    filled.insert(filled.end(), branch_b.begin(), branch_b.end());
    const std::vector<uint8_t> bytes = BgmTestFile({{{file_of(tail_at)}}}, {kBgmPlayPhrase}, filled);

    ultralib::sequence_export::n64::SbnSettings settings;
    const auto first_key = [&](int mix)
    {
        settings.mix = mix;
        const ultralib::sequence_export::n64::SbnPerformance played = PlayBgm(bytes, settings);
        return AllEventsOfKind(played.performance, midi::EventKind::kNoteOn).at(0).key;
    };

    Check(first_key(0) == 60, "the first of a branch's places");
    Check(first_key(1) == 72, "a mix's place");
    Check(first_key(7) == 60, "a mix past the table takes the first place");
}

void TestSbnLinked()
{
    // Track 2 links to track 1. The game's code chooses which of them sounds.
    constexpr uint32_t kLinkedToFirst = 1U << 13 | 1U << 9;
    const std::vector<uint8_t> bytes =
        BgmTestFile({{{BgmCommands({0xBC, 100, 5, 10, 0x00})}, {BgmCommands({0xC8, 100, 5, 20}), kLinkedToFirst}}},
                    {kBgmPlayPhrase});
    const auto keys = [&](bool linked)
    {
        ultralib::sequence_export::n64::SbnSettings settings;
        settings.linked = linked;
        const ultralib::sequence_export::n64::SbnPerformance played = PlayBgm(bytes, settings);
        const std::vector<midi::TrackEvent> ons = AllEventsOfKind(played.performance, midi::EventKind::kNoteOn);
        return ons.size() == 1 ? ons[0].key : -1;
    };

    Check(keys(false) == 60, "a linked track starts muted");
    Check(keys(true) == 72, "the linked track plays in place of the other");
}

void TestSbnChanges()
{
    // The game's code fades tracks to a list of levels a few frames after it starts the song. The note runs past that.
    const std::vector<uint8_t> held = BgmTestFile(
        {{{BgmCommands({0xE0, 0x00, 200, 0xBC, 100, 0x70, 0x77, 0x77, 0x77, 0x77, 0x00})}}}, {kBgmPlayPhrase});
    const auto last_level =
        [](const std::vector<uint8_t>& bytes, const ultralib::sequence_export::n64::SbnSettings& settings)
    {
        const ultralib::sequence_export::n64::SbnPerformance played = PlayBgm(bytes, settings);
        return AllEventsOfKind(played.performance, midi::EventKind::kExpression).back();
    };

    const std::array<uint8_t, 4> levels = {1, 64, 0, 0};
    const std::array<uint8_t, 4> silence = {1, 0, 0, 0};
    ultralib::sequence_export::n64::SbnSettings settings;
    settings.track_levels = levels;
    const midi::TrackEvent faded = last_level(held, settings);
    Check(faded.value == 64, "a list's level");

    // 200 beats a minute at 48 ticks a beat is 160 ticks a second. 0.2 s is then about 32 ticks. The song's first tick
    // runs at the tempo before its first command.
    Check(faded.tick >= 26 + 48 && faded.tick <= 34 + 48, "a list's level takes 48 ticks from about 0.2 s");

    settings.track_levels = silence;
    Check(last_level(held, settings).value == 0, "a list can silence a track");

    // A mix's tracks take the game's level at their first branch. The table sends both values to one place.
    const auto branching = [](std::size_t table)
    {
        const auto hi = static_cast<uint8_t>(table >> 8);
        const auto lo = static_cast<uint8_t>(table);
        return BgmCommands({0xFC, hi, lo, 2, 0, 1, 0xBC, 100, 5, 10, 0x00});
    };

    std::size_t tail_at = 0;
    BgmTestFile({{{branching(0)}}}, {kBgmPlayPhrase}, std::vector<uint8_t>(11, 0), &tail_at);
    const std::size_t place = tail_at + 6;
    const auto hi = static_cast<uint8_t>(place >> 8);
    const auto lo = static_cast<uint8_t>(place);
    const std::vector<uint8_t> mixed =
        BgmTestFile({{{branching(tail_at)}}}, {kBgmPlayPhrase}, {hi, lo, 0, hi, lo, 0, 0xBC, 100, 5, 10, 0x00});
    settings = {};
    settings.mix = 1;
    settings.proximity = 0x57;
    Check(last_level(mixed, settings).value == 0x57, "a mix at the near level");
    settings.proximity = 0x7F;
    Check(last_level(mixed, settings).value == 0x7F, "a mix at the full level");
    settings.mix = 0;
    settings.proximity = 0x57;
    Check(last_level(mixed, settings).value == 0x7F, "the first mix keeps its level");

    // A proximity command with values stores them. One without values gives them to every track, by the level.
    const std::vector<uint8_t> proximity =
        BgmTestFile({{{BgmCommands({0xFF, 6, 100, 50, 0xBC, 100, 20, 30, 0x00})},
                      {BgmCommands({0x02, 0xFF, 6, 0, 0, 0xC8, 100, 20, 30, 0x00})}}},
                    {kBgmPlayPhrase});
    const auto first_track_level = [&](int level)
    {
        ultralib::sequence_export::n64::SbnSettings proximity_settings;
        proximity_settings.mix = 1;
        proximity_settings.proximity = level;
        const ultralib::sequence_export::n64::SbnPerformance played = PlayBgm(proximity, proximity_settings);
        const auto& track = played.performance.tracks.at(0);
        int value = -1;
        for (const midi::TrackEvent& event : track)
        {
            if (event.kind == midi::EventKind::kExpression)
            {
                value = event.value;
            }
        }

        return value;
    };

    Check(first_track_level(0x7F) == 100, "a proximity command at the full level");
    Check(first_track_level(0x57) == 50, "a proximity command at the near level");

    // A composition without a phrase plays nothing, and one that lists the same phrases plays the same.
    std::vector<uint8_t> one = BgmTestFile({{{BgmCommands({0xBC, 100, 5, 10, 0x00})}}}, {kBgmPlayPhrase});
    const auto variations = [](const std::vector<uint8_t>& bytes)
    {
        return ultralib::sequence_export::n64::SbnVariations(n64::BgmFile::Parse(bytes));
    };

    Check(variations(one) == std::vector<int>{0}, "one composition");
    one[0x17] = one[0x15];
    Check(variations(one) == std::vector<int>{0}, "a composition that lists the same phrases");
    one[0x19] = static_cast<uint8_t>(one[0x15] + 1);
    Check(variations(one) == std::vector<int>{0}, "a composition without a phrase");

    std::vector<uint8_t> two =
        BgmTestFile({{{BgmCommands({0xBC, 100, 5, 10, 0x00})}}, {{BgmCommands({0xC8, 100, 5, 10, 0x00})}}},
                    {kBgmPlayPhrase, kBgmPlayPhrase | 1});
    two[0x17] = static_cast<uint8_t>(two[0x15] + 1);
    Check(variations(two) == std::vector<int>({0, 1}), "a composition with other phrases");
}

void TestSbnLevels()
{
    {
        const ultralib::sequence_export::n64::SbnPerformance played = PlayBgm(
            BgmTestFile({{{BgmCommands({0xEC, 100, 0xE9, 64, 0xE1, 127, 0xBC, 100, 5, 10, 0x00})}}}, {kBgmPlayPhrase}));
        const ultralib::sequence_export::n64::Performance& performance = played.performance;
        Check(AllEventsOfKind(performance, midi::EventKind::kVolume).at(0).value == 100, "a track's volume is CC7");
        Check(AllEventsOfKind(performance, midi::EventKind::kExpression).at(0).value == 64,
              "an instrument's volume is CC11");
    }

    {
        // The master volume multiplies in, and the send takes its share from the dry level on the sin/cos law.
        const ultralib::sequence_export::n64::SbnPerformance played =
            PlayBgm(BgmTestFile({{{BgmCommands({0xE1, 64, 0xEB, 40, 0xBC, 100, 5, 10, 0x00})}}}, {kBgmPlayPhrase}));
        const int expected =
            static_cast<int>(std::lround(64.0 * std::sqrt(std::cos(std::numbers::pi / 2.0 * 40.0 / 127.0))));
        const std::vector<midi::TrackEvent> levels = AllEventsOfKind(played.performance, midi::EventKind::kExpression);
        Check(levels.at(0).value == expected, "the master volume and the send in CC11");
        Check(AllEventsOfKind(played.performance, midi::EventKind::kReverb).at(0).value == 40, "a send is CC91");
    }

    {
        // An instrument's volume fades in steps, one for each tick.
        const ultralib::sequence_export::n64::SbnPerformance played =
            PlayBgm(BgmTestFile({{{BgmCommands({0xBC, 100, 50, 0xF6, 0, 10, 27, 20, 0x00})}}}, {kBgmPlayPhrase}));
        const std::vector<midi::TrackEvent> levels = AllEventsOfKind(played.performance, midi::EventKind::kExpression);
        Check(levels.size() > 5 && levels.back().value == 27 && levels.back().tick == 10, "a fade's end");
    }

    {
        // A track's detune is a pitch bend. A coarse tune moves the key.
        const ultralib::sequence_export::n64::SbnPerformance played =
            PlayBgm(BgmTestFile({{{BgmCommands({0xED, 2, 0xEF, 0, 50, 0xBC, 100, 5, 10, 0x00})}}}, {kBgmPlayPhrase}));
        Check(AllEventsOfKind(played.performance, midi::EventKind::kNoteOn).at(0).key == 62, "a coarse tune");
        Check(!AllEventsOfKind(played.performance, midi::EventKind::kPitch).empty(), "a detune's bend");
    }
}

void TestSbnMasterAndInstrument()
{
    {
        // 0xE5 fades the master volume to 0 over 100 ticks. 0xE1 at tick 10 sets it back to full, and the fade ends
        // there.
        const ultralib::sequence_export::n64::SbnPerformance played = PlayBgm(BgmTestFile(
            {{{BgmCommands({0xE5, 0x00, 100, 0, 0xBC, 100, 0x70, 10, 0xE1, 127, 0x77, 0x00})}}}, {kBgmPlayPhrase}));
        const std::vector<midi::TrackEvent> levels = AllEventsOfKind(played.performance, midi::EventKind::kExpression);
        Check(!levels.empty() && levels.back().tick == 10, "0xE1 ends the master volume's fade");
        Check(!levels.empty() && levels.back().value == levels.front().value, "at the level that 0xE1 sets");
    }

    {
        // 0xF4 gives the track a random pan, and 0xF5 then takes an instrument at pan 64. The notes keep the random
        // pan.
        const std::array<n64::BgmInstrument, 1> prg = {{{.bank_patch = 0x3000, .volume = 100, .pan = 64}}};
        std::vector<uint8_t> commands = BgmCommands({0xF4, 64, 127, 0xF5, 0x80});
        for (int note = 0; note < 4; note++)
        {
            commands.insert(commands.end(), {0xBC, 100, 5, 10});
        }

        commands.push_back(0x00);
        const ultralib::sequence_export::n64::SbnPerformance played =
            PlayBgm(BgmTestFile({{{commands}}}, {kBgmPlayPhrase}), {}, prg);
        const std::vector<midi::TrackEvent> pans = AllEventsOfKind(played.performance, midi::EventKind::kPan);
        Check(std::ranges::any_of(pans, [](const midi::TrackEvent& event) { return event.value != 64; }),
              "0xF5 keeps the random pan");
    }
}

void TestSbnPresets()
{
    // A tremolo's rate of 16 goes round in 16 ticks. Its depth comes in cents.
    const ultralib::sequence_export::n64::SbnPerformance played = PlayBgm(
        BgmTestFile({{{BgmCommands({0xEE, 7, 0xF0, 0, 16, 100, 0xBC, 100, 5, 5, 0xF3, 0xBC, 100, 5, 10, 0x00})}}},
                    {kBgmPlayPhrase}));
    Check(played.presets.size() == 2, "a tremolo has a preset of its own");
    const ultralib::sequence_export::n64::SbnPresetKey& key = played.presets.at(0);
    Check(key.bank_patch == 0x3000 && key.fine_cents == 7, "a preset's instrument and fine tune");
    Check(key.tremolo_depth == 100, "a tremolo's depth");

    // 156 beats a minute at 48 ticks a beat is about 124.8 ticks a second.
    Check(Near(key.tremolo_hz_sixteenths / 16.0, 124.8 / 16.0, 0.1), "a tremolo's rate");
    Check(played.presets.at(1).tremolo_depth == 0, "a stopped tremolo");
}

// The engine's table of an envelope's times stands in here with a few values in microseconds.
std::vector<int32_t> SbnTestTimes()
{
    std::vector<int32_t> times(95, 0);
    times[0x4D] = 1000000;
    times[0x50] = 250000;
    times[0x2B] = 500000;

    return times;
}

void TestSbnZones()
{
    const std::vector<int32_t> times = SbnTestTimes();
    constexpr uint32_t kOutput = 32000;
    const std::array<uint8_t, 3> press = {0x5E, 0x7F, 0xFF};
    const std::array<uint8_t, 3> release = {0x4D, 0x00, 0xFF};
    ultralib::sequence_export::n64::SbnSound sound;
    sound.key_base = 6000;
    sound.sample_rate = 16000;
    sound.press = press;
    sound.release = release;

    const std::vector<Zone> zones = ultralib::sequence_export::n64::SbnInstrumentZones(sound, {}, times, kOutput);

    // At 16000 Hz and a key base of 60, key 72 plays the wave at its rate.
    Check(zones.at(0).root_key == 72 && Near(zones.at(0).tune_cents, 0.0, 1e-6), "a zone's root");
    Check(Near(zones.at(0).attack_ms, 0.0, 1e-6) && Near(zones.at(0).decay_ms, 0.0, 1e-6), "a list of one step");

    // The factors at their top reach 31972 of 32768, and the envelope reaches 127 of 128. The mixer squares both.
    const double full = (31972.0 / 32768.0) * (127.0 / 128.0);
    Check(Near(zones.at(0).gain, full * full, 1e-9), "a zone's level");

    // A straight line of amplitude to 0 in a second has the energy of a fall of 100 dB in 10/3 ln(10) seconds.
    Check(Near(zones.at(0).release_ms, 10.0 / 3.0 * std::numbers::ln10 * 1000.0, 1e-6), "a release's time");
    Check(Near(ultralib::sequence_export::n64::SbnReleaseSeconds(release, times, kOutput), 1.0, 1e-6),
          "a voice's release");

    // Keys past the resampler's limit of 2 hold the limit: at key 84 the wave would play at twice its rate.
    Check(zones.size() == 2 && zones[1].low_key == 84 && !zones[1].follows_key, "the resampler's limit");
    Check(zones[0].high_key == 83, "keys below the limit");

    // A release in steps relative to the level at the key's release: four lines of a quarter second each. The mixer
    // moves in a straight line of amplitude to the square of each level.
    const std::array<uint8_t, 9> steps = {0x50, 0xDF, 0x50, 0xBF, 0x50, 0x9F, 0x50, 0x80, 0xFF};
    sound.release = steps;
    const std::vector<Zone> stepped = ultralib::sequence_export::n64::SbnInstrumentZones(sound, {}, times, kOutput);
    double energy = 0.0;
    double before = 1.0;
    for (const double level : {95.0 / 128.0, 63.0 / 128.0, 31.0 / 128.0, 0.0})
    {
        const double next = level * level;
        energy += 0.25 * (before * before + before * next + next * next) / 3.0;
        before = next;
    }

    Check(Near(stepped.at(0).release_ms, 10.0 * std::numbers::ln10 * energy * 1000.0, 1e-6), "a release in steps");
    Check(Near(ultralib::sequence_export::n64::SbnReleaseSeconds(steps, times, kOutput), 1.0, 1e-6),
          "a list's whole time");

    // A press list with a decay.
    const std::array<uint8_t, 7> decays = {0x5E, 0x7F, 0x2B, 0x7F, 0x4D, 0x00, 0xFF};
    sound.press = decays;
    sound.release = release;
    const std::vector<Zone> decaying = ultralib::sequence_export::n64::SbnInstrumentZones(sound, {}, times, kOutput);
    Check(decaying.at(0).decay_ms > 0.0 && Near(decaying.at(0).sustain_db, 100.0, 1e-6),
          "a list that falls to nothing");

    // The first list that a song writes replaces the instrument's.
    ultralib::sequence_export::n64::SbnPresetKey custom;
    const std::vector<uint8_t> custom_press = {0x4D, 0x7F, 0xFF, 0x00};
    custom.press = custom_press;
    const std::vector<Zone> rising = ultralib::sequence_export::n64::SbnInstrumentZones(sound, custom, times, kOutput);
    Check(Near(rising.at(0).attack_ms, 1000.0, 1e-6), "a custom press list");

    // A track of one voice cuts by an exclusive class.
    ultralib::sequence_export::n64::SbnPresetKey cuts;
    cuts.cuts = true;
    Check(ultralib::sequence_export::n64::SbnInstrumentZones(sound, cuts, times, kOutput).at(0).exclusive_class != 0,
          "a class for one voice");
}

void TestSbnDrumZone()
{
    const std::vector<int32_t> times = SbnTestTimes();
    constexpr uint32_t kOutput = 32000;
    const std::array<uint8_t, 3> press = {0x5E, 0x7F, 0xFF};
    const std::array<uint8_t, 3> release = {0x4D, 0x00, 0xFF};
    ultralib::sequence_export::n64::SbnSound sound;
    sound.key_base = 4800;
    sound.sample_rate = 32000;
    sound.press = press;
    sound.release = release;

    n64::BgmDrum drum;
    drum.key_base = 4900;
    drum.volume = 64;
    drum.pan = 127;
    const Zone zone = ultralib::sequence_export::n64::SbnDrumZone(sound, drum, 40, 0, times, kOutput);
    Check(zone.low_key == 40 && zone.high_key == 40 && zone.root_key == 40, "a drum's key");
    Check(!zone.follows_key && Near(zone.tune_cents, 100.0, 1e-6), "a drum's pitch");
    Check(Near(zone.pan, 1.0, 1e-6), "a drum's pan");

    const double full = (31972.0 / 32768.0) * (127.0 / 128.0) * (64.0 / 128.0);
    Check(Near(zone.gain, full * full, 1e-9), "a drum's level");

    // A drum's reverb takes its share from the dry level in the zone.
    drum.reverb = 127;
    Check(Near(ultralib::sequence_export::n64::SbnDrumZone(sound, drum, 40, 0, times, kOutput).gain, 0.0, 1e-9),
          "a drum's send");
}

void TestSbnDrumNote()
{
    // Keys below 72 name the game's drums, and the others the song's.
    n64::BgmDrum drum;
    drum.bank_patch = 0x30B0;
    drum.key_base = 4800;
    drum.volume = 127;
    drum.reverb = 25;
    const std::vector<n64::BgmDrum> per(72, drum);
    constexpr uint32_t kDrums = 1U << 13 | 0x80;
    const std::vector<uint8_t> bytes = BgmTestFile({{{{0x80 + 40, 100, 5, 10, 0x00}, kDrums}}}, {kBgmPlayPhrase});
    const n64::BgmFile file = n64::BgmFile::Parse(bytes);
    const ultralib::sequence_export::n64::SbnFacts facts = [](uint16_t bank_patch)
    {
        Check(bank_patch == 0x30B0, "a drum's instrument");
        return ultralib::sequence_export::n64::SbnInstrumentFacts{
            .exists = true, .key_base = 4800, .sample_rate = 32000, .loops = true};
    };

    const ultralib::sequence_export::n64::SbnPerformance played =
        ultralib::sequence_export::n64::PerformSbn(file, per, {}, facts, {}, {});
    Check(played.presets.at(0).drums && played.drum_keys.count(40) == 1, "a drum's preset");
    Check(AllEventsOfKind(played.performance, midi::EventKind::kNoteOn).at(0).key == 40, "a drum's key");
    Check(AllEventsOfKind(played.performance, midi::EventKind::kReverb).at(0).value == 25, "a drum's send");
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("sequence_export_n64_sbn",
                                    []()
                                    {
                                        TestSbnClock();
                                        TestSbnShortNote();
                                        TestSbnVoices();
                                        TestSbnComposition();
                                        TestSbnMix();
                                        TestSbnLinked();
                                        TestSbnChanges();
                                        TestSbnLevels();
                                        TestSbnMasterAndInstrument();
                                        TestSbnPresets();
                                        TestSbnZones();
                                        TestSbnDrumZone();
                                        TestSbnDrumNote();
                                    });
}
