// SPDX-License-Identifier: MIT

// Unit tests for the conversion of the songs of the N64 Music Tools. The tests build small sound files in memory. Their
// expected values follow from the arithmetic of the engines, worked out by hand.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"
#include "xsflib/testing/near.h"

#include "n64_bytes.h"
#include "sequence_export_n64_testing.h"
#include "ultralib/sequence_export/n64/games.h"
#include "ultralib/sequence_export/n64/libmus.h"

namespace
{

namespace n64 = ultralib::n64;
namespace midi = seqlib::midi;
using ultralib::sequence_export::n64::Zone;
using ultralib::testing::AllEventsOfKind;
using ultralib::testing::EventsOfKind;
using ultralib::testing::PutAt;
using xsflib::testing::Check;
using xsflib::testing::Near;

// A song file of the N64 Music Tools. Each list starts at its offset. A channel's volume and bend lists may be empty.
struct MusChannel
{
    std::vector<uint8_t> data;
    std::vector<uint8_t> volume = {};
    std::vector<uint8_t> bend = {};
};

std::vector<uint8_t> MusSong(const std::vector<MusChannel>& channels, const std::vector<uint8_t>& master,
                             const std::vector<uint8_t>& envelopes = {}, const std::vector<uint8_t>& drums = {},
                             const std::vector<uint16_t>& waves = {0, 1})
{
    constexpr uint32_t kLists = 0x2C;
    const uint32_t count = static_cast<uint32_t>(channels.size());
    std::vector<uint8_t> song(kLists + 12 * count);
    const auto append = [&](const std::vector<uint8_t>& bytes)
    {
        const uint32_t at = static_cast<uint32_t>(song.size());
        song.insert(song.end(), bytes.begin(), bytes.end());

        return at;
    };

    PutAt(song, 0x04, count);
    PutAt(song, 0x08, static_cast<uint32_t>(waves.size()));
    PutAt(song, 0x0C, kLists);
    PutAt(song, 0x10, kLists + 4 * count);
    PutAt(song, 0x14, kLists + 8 * count);
    for (uint32_t i = 0; i < count; i++)
    {
        const MusChannel& channel = channels[i];
        PutAt(song, kLists + 4 * i, append(channel.data));
        PutAt(song, kLists + 4 * count + 4 * i, channel.volume.empty() ? 0 : append(channel.volume));
        PutAt(song, kLists + 8 * count + 4 * i, channel.bend.empty() ? 0 : append(channel.bend));
    }

    PutAt(song, 0x18, append(envelopes));
    PutAt(song, 0x1C, append(drums));
    std::vector<uint8_t> wave_bytes;
    for (const uint16_t wave : waves)
    {
        xsflib::AppendBe16(wave_bytes, wave);
    }

    PutAt(song, 0x20, append(wave_bytes));
    PutAt(song, 0x24, append(master));

    return song;
}

// A master track that sets a script tempo of 120 and rests for `ticks`, in an endless loop.
std::vector<uint8_t> MusMaster(uint8_t high, uint8_t low)
{
    return {0x85, 120, 0x9A, 0x95, 0xFF, 96, static_cast<uint8_t>(high | 0x80), low, 0x96};
}

constexpr std::array<double, 2> kMusDetunes = {-48.0, -47.8};

void TestLibmusNote()
{
    // velon, an endit of 1, a loop of a note of 96 ticks and a rest of 96.
    const std::vector<uint8_t> song =
        MusSong({{.data = {0x99, 0x87, 1, 0x95, 0xFF, 48, 100, 96, 96, 0, 96, 0x96}}}, MusMaster(0, 192));
    const ultralib::sequence_export::n64::LibmusPerformance played =
        ultralib::sequence_export::n64::PerformLibmus(song, kMusDetunes, {}, {});
    const ultralib::sequence_export::n64::Performance& performance = played.performance;

    // 120 * 256 * 96 / 120 / 60 falls to 409, with 256 for a tick. A callback's 16666 microseconds come to 367 samples
    // at the rate of 22050.
    const double ticks_per_second = 409.0 * (22050.0 / 367.0) / 256.0;
    Check(Near(played.ticks_per_second, ticks_per_second, 1e-6), "the ticks a second");
    Check(performance.tempo.size() == 1 && Near(performance.tempo[0].bpm, ticks_per_second * 60.0 / 48.0, 1e-6),
          "the tempo");
    Check(performance.timebase == 48, "the file's ticks");
    Check(performance.loop.has_value() && performance.loop->start == 0 && performance.loop->end == 192, "the loop");
    Check(performance.end_tick == 192, "the end of the pass");

    const std::vector<midi::TrackEvent> ons = EventsOfKind(performance, midi::EventKind::kNoteOn);
    Check(ons.size() == 1 && ons[0].tick == 0 && ons[0].key == 60 && ons[0].value == 100, "the note");
    const std::vector<midi::TrackEvent> offs = EventsOfKind(performance, midi::EventKind::kNoteOff);
    Check(offs.size() == 1 && offs[0].tick == 95, "the release starts an endit before the note's end");
    Check(played.presets.size() == 1 && played.presets[0].wave == 0, "the wave");
}

void TestLibmusVelocity()
{
    // A velocity of 0x80 or more becomes the default, and the notes after it have no velocity byte.
    {
        const std::vector<uint8_t> song =
            MusSong({{.data = {0x99, 0x95, 0xFF, 48, 90, 48, 50, 0xC0, 48, 52, 48, 0x96}}}, MusMaster(0, 144));
        const ultralib::sequence_export::n64::LibmusPerformance played =
            ultralib::sequence_export::n64::PerformLibmus(song, kMusDetunes, {}, {});
        const std::vector<midi::TrackEvent> ons = EventsOfKind(played.performance, midi::EventKind::kNoteOn);
        Check(ons.size() == 3 && ons[0].value == 90 && ons[1].value == 0x40 && ons[2].value == 0x40,
              "the default velocity");
    }

    // A voice's volume stops at 32767. 2^28 / (127 * 127 * 240) falls to 69.
    {
        const std::vector<uint8_t> song = MusSong({{.data = {0x99, 0x95, 0xFF, 48, 100, 48, 48, 60, 48, 0x96}}},
                                                  MusMaster(0, 96), {1, 127, 1, 127, 1, 127, 10});
        const ultralib::sequence_export::n64::LibmusPerformance played =
            ultralib::sequence_export::n64::PerformLibmus(song, kMusDetunes, {.song_volume = 240}, {});
        const std::vector<midi::TrackEvent> ons = EventsOfKind(played.performance, midi::EventKind::kNoteOn);
        Check(ons.size() == 2 && ons[0].value == 69 && ons[1].value == 60, "the top of the volume");
    }

    // Note 61 on a detune of -48 is over an octave above the output's rate. The player silences it, and a velocity of 0
    // says the same. Each of them still cuts the note before.
    {
        const std::vector<uint8_t> song =
            MusSong({{.data = {0x99, 0x95, 0xFF, 61, 100, 48, 60, 100, 48, 48, 0, 48, 0x96}}}, MusMaster(0, 144));
        const ultralib::sequence_export::n64::LibmusPerformance played =
            ultralib::sequence_export::n64::PerformLibmus(song, kMusDetunes, {}, {});
        const std::vector<midi::TrackEvent> ons = EventsOfKind(played.performance, midi::EventKind::kNoteOn);
        Check(ons.size() == 3 && ons[0].value == 1 && ons[1].value == 100 && ons[2].value == 1, "silent notes");
    }
}

void TestLibmusDrums()
{
    // The table's entries are a wave, an envelope, a pan and a pitch. The song's wave table leads to the bank's wave.
    const std::vector<uint8_t> envelopes = {1, 127, 1, 127, 1, 127, 10, 1, 90, 1, 90, 1, 90, 20};
    const std::vector<uint8_t> drums = {0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 100, 50};
    const std::vector<uint8_t> song = MusSong(
        {{.data = {0x99, 0x9E, 0, 0x95, 0xFF, 1, 100, 48, 0x9F, 0x9C, 40, 0x81, 0, 0x90, 0, 48, 100, 48, 0x96}}},
        MusMaster(0, 96), envelopes, drums, {7, 1});
    const std::array<double, 8> detunes = {-48.0, -47.8, -48.0, -48.0, -48.0, -48.0, -48.0, -48.0};
    const ultralib::sequence_export::n64::LibmusPerformance played =
        ultralib::sequence_export::n64::PerformLibmus(song, detunes, {}, {});
    const ultralib::sequence_export::n64::Performance& performance = played.performance;

    const std::vector<midi::TrackEvent> ons = EventsOfKind(performance, midi::EventKind::kNoteOn);
    Check(ons.size() == 2 && ons[0].key == 62 && ons[1].key == 60, "the drum's pitch and the note");
    const std::vector<midi::TrackEvent> pans = EventsOfKind(performance, midi::EventKind::kPan);
    Check(pans.size() == 2 && pans[0].value == 50 && pans[1].value == 20, "half of each pan");
    Check(played.presets.size() == 2, "a preset for each sound");
    Check(played.presets[0].wave == 1 && played.presets[0].envelope[1] == 90, "the drum's sound");
    Check(played.presets[1].wave == 7 && played.presets[1].envelope[1] == 127, "the note's sound");
}

void TestLibmusLists()
{
    // The list's values come a tick apart. Above 127, a count follows: the value lasts for the count plus 2 ticks.
    const std::vector<uint8_t> song = MusSong({{.data = {0x99, 0x95, 0xFF, 48, 100, 8, 0x96},
                                                .volume = {100, 0x80 | 90, 1, 0x80 | 80, 20},
                                                .bend = {64, 0x80 | 32, 1, 0x80 | 96, 20}}},
                                              MusMaster(0, 8));
    const ultralib::sequence_export::n64::LibmusPerformance played =
        ultralib::sequence_export::n64::PerformLibmus(song, kMusDetunes, {}, {});
    const ultralib::sequence_export::n64::Performance& performance = played.performance;

    std::vector<std::pair<uint32_t, int>> volumes;
    for (const midi::TrackEvent& event : EventsOfKind(performance, midi::EventKind::kVolume))
    {
        volumes.emplace_back(event.tick, event.value);
    }

    const std::vector<std::pair<uint32_t, int>> expected_volumes = {{0, 100}, {1, 90}, {4, 80}};
    Check(volumes == expected_volumes, "the volume list");

    std::vector<std::pair<uint32_t, float>> bends;
    for (const midi::TrackEvent& event : EventsOfKind(performance, midi::EventKind::kPitch))
    {
        bends.emplace_back(event.tick, event.semitones);
    }

    // (value - 64) * 2 / 64 semitones.
    const std::vector<std::pair<uint32_t, float>> expected_bends = {{1, -1.0F}, {4, 1.0F}};
    Check(bends == expected_bends, "the bend list");
}

void TestLibmusLoops()
{
    // The channels enter their endless loops at different ticks, with different lengths. The main loop starts when the
    // last is in, and it lasts until they meet.
    const std::vector<uint8_t> song = MusSong({{.data = {0x99, 0x95, 0xFF, 48, 100, 96, 0x96}},
                                               {.data = {0x99, 48, 100, 48, 0x95, 0xFF, 50, 100, 0x80, 144, 0x96}}},
                                              MusMaster(0, 96));
    const ultralib::sequence_export::n64::LibmusPerformance played =
        ultralib::sequence_export::n64::PerformLibmus(song, kMusDetunes, {}, {});
    const ultralib::sequence_export::n64::Performance& performance = played.performance;
    Check(performance.loop.has_value() && performance.loop->start == 48 && performance.loop->end == 48 + 288,
          "the main loop");

    // The first channel plays at 0, 96, 192 and 288, and the second at 0, 48 and 192. The pass ends before 336.
    const std::vector<midi::TrackEvent> ons = EventsOfKind(performance, midi::EventKind::kNoteOn);
    Check(ons.size() == 4 && ons[3].tick == 288, "the first channel's notes");
    std::vector<uint32_t> second;
    for (const midi::TrackEvent& event : performance.tracks[1])
    {
        if (event.kind == midi::EventKind::kNoteOn)
        {
            second.push_back(event.tick);
        }
    }

    Check(second == std::vector<uint32_t>{0, 48, 192}, "the second channel's notes");
}

void TestLibmusZone()
{
    ultralib::sequence_export::n64::LibmusSettings settings;
    settings.song_volume = 128;
    settings.master_volume = 18021;
    const ultralib::sequence_export::n64::LibmusPresetKey key = {.wave = 0, .envelope = {1, 100, 6, 127, 28, 89, 54}};
    const Zone zone = ultralib::sequence_export::n64::LibmusZone(key, -47.8, settings, 96.0);

    // Key 60 plays note 48, at 0.2 semitones above the output's rate.
    Check(zone.root_key == 60 && Near(zone.tune_cents, 20.0, 1e-6), "the tuning");
    Check(zone.exclusive_class == 1, "a new note cuts the last");

    // The rise's energy, for a square from 100/127 to 1 over 6 ticks at 96 a second.
    const double first = 100.0 / 127.0;
    const double energy = (1.0 + first + first * first) / 3.0;
    Check(Near(zone.attack_ms, 2.0 * (1.0 - energy) * 6.0 * 1000.0 / 96.0, 1e-6), "the attack");
    Check(Near(zone.delay_ms, 0.0, 1e-6), "no delay");
    Check(Near(zone.sustain_db, -20.0 * std::log10((89.0 / 127.0) * (89.0 / 127.0)), 1e-6), "the sustain");

    // A release in a straight line, squared, from any level to nothing in 54 ticks.
    Check(Near(zone.release_ms, 2.0 * std::numbers::ln10 * 54.0 * 1000.0 / 96.0, 1e-6), "the release");

    // 127^3 * 128 / 2^13 of 32768, times the master's 18021 of 32768, squared by the mixer.
    const double full = 127.0 * 127.0 * 127.0 * 128.0 / 8192.0 / 32768.0 * (18021.0 / 32768.0);
    Check(Near(zone.gain, full * full, 1e-9), "the gain");
}

void TestLibmusSharedTrack()
{
    // Seventeen channels with notes: two share a track. The two that never sound together share.
    std::vector<MusChannel> channels;
    channels.push_back({.data = {0x99, 0x95, 0xFF, 48, 100, 48, 96, 0, 48, 0x96}});
    channels.push_back({.data = {0x99, 0x95, 0xFF, 96, 0, 48, 50, 100, 48, 0x96}});
    for (int i = 2; i < 17; i++)
    {
        channels.push_back({.data = {0x99, 0x95, 0xFF, 52, 100, 96, 0x96}});
    }

    const std::vector<uint8_t> song = MusSong(channels, MusMaster(0, 96));
    const ultralib::sequence_export::n64::LibmusPerformance played =
        ultralib::sequence_export::n64::PerformLibmus(song, kMusDetunes, {}, {});
    const ultralib::sequence_export::n64::Performance& performance = played.performance;
    Check(performance.tracks[0].size() > performance.tracks[1].size(), "the first track holds both");
    std::vector<int> keys;
    for (const midi::TrackEvent& event : performance.tracks[0])
    {
        if (event.kind == midi::EventKind::kNoteOn)
        {
            keys.push_back(event.key);
        }
    }

    Check(keys == std::vector<int>{60, 62}, "their notes");
    Check(!performance.approximations.empty(), "the report says so");
}

void TestUnpackDirectoryFile()
{
    const auto expect_throw = [](std::vector<uint8_t> bytes, const char* what)
    {
        bool thrown = false;
        try
        {
            ultralib::sequence_export::n64::UnpackDirectoryFile(bytes);
        }
        catch (const xsflib::FormatError&)
        {
            thrown = true;
        }

        Check(thrown, what);
    };

    // Four bytes, then a copy of eight from a distance of 4, then the end.
    const std::vector<uint8_t> short_copy = {1, 0, 0, 12, 0x0C, 'A', 'B', 'C', 'D', 0x07, 0x04, 0, 0};
    const std::vector<uint8_t> abcd = ultralib::sequence_export::n64::UnpackDirectoryFile(short_copy);
    Check(std::string(abcd.begin(), abcd.end()) == "ABCDABCDABCD", "a short copy");

    // A third byte of 3 gives a length of 20.
    const std::vector<uint8_t> run = ultralib::sequence_export::n64::UnpackDirectoryFile(
        std::vector<uint8_t>{1, 0, 0, 21, 0x60, 'A', 0, 1, 3, 0, 0});
    Check(run.size() == 21 && run.back() == 'A', "a long copy");

    // The first byte's high nibble holds the distance's top bits: 0x112 reaches back to the start here.
    const std::vector<uint8_t> far = ultralib::sequence_export::n64::UnpackDirectoryFile(
        std::vector<uint8_t>{1, 0, 1, 0x14, 0x38, 'Q', 'X', 0, 1, 0xFF, 0x11, 0x12, 0, 0});
    Check(far.size() == 0x114 && far[0x112] == 'Q' && far[0x113] == 'X', "a far copy");

    // A group of eight bytes with no flags set, then a second group.
    const std::vector<uint8_t> groups = ultralib::sequence_export::n64::UnpackDirectoryFile(
        std::vector<uint8_t>{1, 0, 0, 9, 0, '1', '2', '3', '4', '5', '6', '7', '8', 0x40, '9', 0, 0});
    Check(std::string(groups.begin(), groups.end()) == "123456789", "two groups");

    expect_throw({0, 0, 0, 1, 0x40, 'A', 0, 0}, "a file of another kind");
    expect_throw({1, 0, 0, 2, 0x40, 'A', 0, 0}, "a size that isn't the file's");
    expect_throw({1, 0, 0, 2, 0x40, 'A', 0x01, 0x05}, "a copy from before the start");
    expect_throw({1, 0, 0, 1, 0x00, 'A'}, "a file with no end");
}

void TestLibmusTempoScale()
{
    const std::vector<uint8_t> song =
        MusSong({{.data = {0x99, 0x87, 1, 0x95, 0xFF, 48, 100, 96, 96, 0, 96, 0x96}}}, MusMaster(0, 192));

    // The script's tempo of 120 comes to 409. The library multiplies that by 0x78 / 128 in whole numbers: 383.
    const double scaled = 383.0 * (22050.0 / 367.0) / 256.0;
    ultralib::sequence_export::n64::LibmusSettings settings;
    settings.tempo_scale = 0x78;
    const ultralib::sequence_export::n64::LibmusPerformance whole =
        ultralib::sequence_export::n64::PerformLibmus(song, kMusDetunes, settings, {});
    Check(Near(whole.ticks_per_second, scaled, 1e-6), "a whole song's scale reaches its envelopes' times");
    Check(whole.performance.tempo.size() == 1 && Near(whole.performance.tempo[0].bpm, scaled * 60.0 / 48.0, 1e-6),
          "a whole song's scale");
}

void TestLibmusRepeats()
{
    // An image with a bank of no waves and four songs. The first two have the same data and volume. The third has
    // another volume. The fourth has the first's data and volume in another bank.
    constexpr std::size_t kSongs = 0x100;
    constexpr std::size_t kVolumes = 0x140;
    constexpr std::size_t kData = 0x180;
    constexpr std::size_t kFirstBank = 0x200;
    constexpr std::size_t kSecondBank = 0x300;
    std::vector<uint8_t> image(0x400, 0);
    for (std::size_t song = 0; song < 4; song++)
    {
        PutAt(image, kSongs + song * 8, kData);
        PutAt(image, kSongs + song * 8 + 4, kData + 16);
        PutAt(image, kVolumes + song * 4, song == 2 ? 0x00728080 : 0x00808080);
    }

    namespace n64_export = ultralib::sequence_export::n64;
    const std::array banks = {
        n64_export::LibmusBankPlace{.first_song = 0, .pointers = kFirstBank, .samples = kFirstBank + 0x80},
        n64_export::LibmusBankPlace{.first_song = 3, .pointers = kSecondBank, .samples = kSecondBank + 0x80},
    };
    const n64_export::LibmusProfile profile = {
        .songs = kSongs, .song_count = 4, .song_volumes = kVolumes, .banks = banks};
    const auto game = n64_export::LibmusGame::Open(image, profile);
    std::vector<std::size_t> indices;
    for (const auto& sequence : game->Sequences())
    {
        indices.push_back(sequence.index);
    }

    Check(indices == std::vector<std::size_t>({0, 2, 3}), "a song that repeats an earlier one is left out");
}

void TestLibmusSongTables()
{
    // Three songs of 16 bytes, each with another first byte, and a bank of no waves.
    constexpr std::size_t kTable = 0x100;
    constexpr std::size_t kSongFiles = 0x140;
    constexpr std::size_t kEnds = 0x160;
    constexpr std::size_t kBase = 0x180;
    constexpr std::size_t kBank = 0x200;
    std::vector<uint8_t> image(0x300, 0);
    for (std::size_t song = 0; song < 3; song++)
    {
        image[kBase + 16 * song] = static_cast<uint8_t>(song + 1);
    }

    namespace n64_export = ultralib::sequence_export::n64;
    const std::array kBanks = {n64_export::LibmusBankPlace{.pointers = kBank, .samples = kBank + 0x80}};

    const auto indices = [](const n64_export::LibmusGame& game)
    {
        std::vector<std::size_t> found;
        for (const auto& sequence : game.Sequences())
        {
            found.push_back(sequence.index);
        }

        return found;
    };

    // A table of files: file 0 is no song, and files 1 to 3 start at 0x00, 0x10 and 0x20 from the base. The game's
    // songs 1 to 4 play files 3, 1, 3 and 2. Song 3 repeats song 1.
    for (std::size_t file = 1; file < 4; file++)
    {
        PutAt(image, kTable + 8 * file, static_cast<uint32_t>(16 * (file - 1)));
        PutAt(image, kTable + 8 * file + 4, 16);
    }

    const std::array<uint16_t, 5> kNumbers = {0, 3, 1, 3, 2};
    for (std::size_t i = 0; i < kNumbers.size(); i++)
    {
        image[kSongFiles + 2 * i] = static_cast<uint8_t>(kNumbers[i] >> 8);
        image[kSongFiles + 2 * i + 1] = static_cast<uint8_t>(kNumbers[i]);
    }

    const n64_export::LibmusProfile files = {
        .song_count = 4,
        .cartridge_files = {.table = kTable, .base = kBase, .song_files = kSongFiles, .first_song = 1},
        .banks = kBanks};

    Check(indices(*n64_export::LibmusGame::Open(image, files)) == std::vector<std::size_t>({1, 2, 4}),
          "the songs of a table of files keep their numbers in the game");

    // A table of starts and a table of ends.
    for (std::size_t song = 0; song < 3; song++)
    {
        PutAt(image, kTable + 4 * song, static_cast<uint32_t>(kBase + 16 * song));
        PutAt(image, kEnds + 4 * song, static_cast<uint32_t>(kBase + 16 * song + 16));
    }

    const n64_export::LibmusProfile ends = {.songs = kTable, .song_count = 3, .song_ends = kEnds, .banks = kBanks};

    Check(indices(*n64_export::LibmusGame::Open(image, ends)) == std::vector<std::size_t>({0, 1, 2}),
          "a table of starts and a table of ends");
}

// The reverb base that a game's code gives a song raises its channels' effect levels toward full.
void TestLibmusReverbBase()
{
    // velon, an endit of 1, a reverb of 64, and a loop of a note of 96 ticks and a rest of 96.
    const std::vector<uint8_t> song =
        MusSong({{.data = {0x99, 0x87, 1, 0xA2, 64, 0x95, 0xFF, 48, 100, 96, 96, 0, 96, 0x96}}}, MusMaster(0, 192));
    const auto send = [&song](int base)
    {
        ultralib::sequence_export::n64::LibmusSettings settings;
        settings.reverb_base = base;
        const ultralib::sequence_export::n64::LibmusPerformance played =
            ultralib::sequence_export::n64::PerformLibmus(song, kMusDetunes, settings, {});

        return AllEventsOfKind(played.performance, midi::EventKind::kReverb).at(0).value;
    };

    Check(send(0) == 64, "no base keeps the song's level");

    // 25 + (128 - 25) * 64 / 128 = 76, in whole numbers.
    Check(send(25) == 76, "a base of 25");
}

// A profile that places each song reads it as it stands or from a raw DEFLATE stream.
void TestLibmusSongList()
{
    // A song of 16 bytes at 0x100, the same bytes in a stored DEFLATE block at 0x120, and a bank of no waves at 0x200.
    std::vector<uint8_t> image(0x300, 0);
    for (std::size_t i = 0; i < 16; i++)
    {
        image[0x100 + i] = static_cast<uint8_t>(i + 1);
        image[0x125 + i] = static_cast<uint8_t>(i + 1);
    }

    // The final block, stored, with its length and the length's complement.
    const std::array<uint8_t, 5> stored = {0x01, 16, 0, 0xEF, 0xFF};
    std::copy(stored.begin(), stored.end(), image.begin() + 0x120);

    namespace n64_export = ultralib::sequence_export::n64;
    const std::array kBanks = {n64_export::LibmusBankPlace{.pointers = 0x200, .samples = 0x280}};
    const std::array kSongs = {
        n64_export::LibmusSong{.start = 0x100, .end = 0x110},
        n64_export::LibmusSong{.start = 0x120, .end = 0x135, .deflated = true},
        n64_export::LibmusSong{.start = 0x120, .end = 0x135, .deflated = true, .reverb_base = 25},
        n64_export::LibmusSong{.start = 0x100, .end = 0x110, .volume = 0x60},
    };

    const n64_export::LibmusProfile profile = {.banks = kBanks, .song_list = kSongs};
    std::vector<std::string> names;
    for (const auto& sequence : n64_export::LibmusGame::Open(image, profile)->Sequences())
    {
        names.push_back(sequence.name);
    }

    Check(names == std::vector<std::string>({"Sequence 0x00", "Sequence 0x02", "Sequence 0x03"}),
          "a packed song with the same bytes, volume and base repeats a plain one");

    image[0x120] = 0x07; // a block of the reserved type
    bool threw = false;
    try
    {
        n64_export::LibmusGame::Open(image, profile);
    }
    catch (const xsflib::FormatError&)
    {
        threw = true;
    }

    Check(threw, "a corrupt DEFLATE stream throws FormatError");
}

// A profile whose table runs past the image throws FormatError. The other reads throw it too. The tables are those of
// the starts and the ends, of the ends, of the songs' files, and the directory.
void TestLibmusTablesOutsideImage()
{
    namespace n64_export = ultralib::sequence_export::n64;
    std::vector<uint8_t> image(0x100, 0);
    image[0xFF] = 0x40; // the song's start in a table at 0xFC
    const std::array kBanks = {n64_export::LibmusBankPlace{.pointers = 0x80, .samples = 0xC0}};
    const auto error_of = [&image](const n64_export::LibmusProfile& profile)
    {
        try
        {
            n64_export::LibmusGame::Open(image, profile);
        }
        catch (const xsflib::FormatError& e)
        {
            return std::string(e.what());
        }

        return std::string();
    };

    const std::string outside = "a table of the game's profile isn't in the image";

    Check(error_of({.songs = 0xFC, .song_count = 1, .banks = kBanks}) == outside, "a table of starts and ends");
    Check(error_of({.songs = 0xF0, .song_count = 1, .song_ends = 0x100, .banks = kBanks}) == outside,
          "a table of ends");
    Check(
        error_of({.song_count = 1, .cartridge_files = {.table = 0x10, .song_files = 0xFF}, .banks = kBanks}) == outside,
        "a table of the songs' files");
    Check(error_of({.directory = 0x100, .banks = kBanks}) == outside, "a directory");
}

void TestLibmusSend()
{
    // A send of 60 goes to CC91. The mixer takes its share from the dry level, and CC11 acts squared.
    const std::vector<uint8_t> song =
        MusSong({{.data = {0x99, 0xA2, 60, 0x95, 0xFF, 48, 100, 96, 96, 0, 96, 0x96}}}, MusMaster(0, 192));
    const ultralib::sequence_export::n64::LibmusPerformance played =
        ultralib::sequence_export::n64::PerformLibmus(song, kMusDetunes, {}, {});
    const std::vector<midi::TrackEvent> sends = EventsOfKind(played.performance, midi::EventKind::kReverb);
    const std::vector<midi::TrackEvent> levels = EventsOfKind(played.performance, midi::EventKind::kExpression);
    Check(!sends.empty() && sends.back().value == 60, "a send");
    const double dry = std::cos(std::numbers::pi / 2.0 * 60.0 / 127.0);
    Check(!levels.empty() && levels.back().value == std::lround(127.0 * std::sqrt(dry)), "the dry level of a send");
}

// A song of the library's early form: its lists start at offset 0 of the header, and it has no master track and no
// table of waves.
std::vector<uint8_t> MusEarlySong(const std::vector<MusChannel>& channels, const std::vector<uint8_t>& envelopes = {})
{
    constexpr uint32_t kLists = 0x18;
    const uint32_t count = static_cast<uint32_t>(channels.size());
    std::vector<uint8_t> song(kLists + 12 * count);
    const auto append = [&](const std::vector<uint8_t>& bytes)
    {
        const uint32_t at = static_cast<uint32_t>(song.size());
        song.insert(song.end(), bytes.begin(), bytes.end());

        return at;
    };

    PutAt(song, 0x00, count);
    PutAt(song, 0x04, kLists);
    PutAt(song, 0x08, kLists + 4 * count);
    PutAt(song, 0x0C, kLists + 8 * count);
    for (uint32_t i = 0; i < count; i++)
    {
        const MusChannel& channel = channels[i];
        PutAt(song, kLists + 4 * i, append(channel.data));
        PutAt(song, kLists + 4 * count + 4 * i, channel.volume.empty() ? 0 : append(channel.volume));
        PutAt(song, kLists + 8 * count + 4 * i, channel.bend.empty() ? 0 : append(channel.bend));
    }

    PutAt(song, 0x10, append(envelopes));

    return song;
}

// Plays a song of the early form, on the library's early player or on its later one.
ultralib::sequence_export::n64::LibmusPerformance PlayEarlySong(const std::vector<uint8_t>& song,
                                                                bool early_player = true)
{
    ultralib::sequence_export::n64::LibmusSettings settings;
    settings.early_form = true;
    settings.early_player = early_player;

    return ultralib::sequence_export::n64::PerformLibmus(song, kMusDetunes, settings, {});
}

// The pitch that a song's last pitch command at or before `tick` sets, in semitones.
double PitchAt(const ultralib::sequence_export::n64::Performance& performance, uint32_t tick)
{
    double pitch = 0.0;
    for (const midi::TrackEvent& event : EventsOfKind(performance, midi::EventKind::kPitch))
    {
        if (event.tick <= tick)
        {
            pitch = event.semitones;
        }
    }

    return pitch;
}

void TestLibmusEarlySong()
{
    // A channel starts without velocities, at a velocity of 127. Wave 1 is the bank's wave 1. An envelope's first byte
    // doesn't count. No wave's table or master track stands behind the note.
    const std::vector<uint8_t> envelopes = {5, 100, 6, 127, 28, 89, 54};
    const ultralib::sequence_export::n64::LibmusPerformance played =
        PlayEarlySong(MusEarlySong({{.data = {0x85, 120, 0x81, 1, 0x90, 0, 48, 96, 0x80}}}, envelopes));

    const std::vector<midi::TrackEvent> ons = EventsOfKind(played.performance, midi::EventKind::kNoteOn);
    Check(ons.size() == 1 && ons[0].key == 60 && ons[0].value == 127, "the early form's note");
    Check(played.presets.size() == 1 && played.presets[0].wave == 1, "the early form's wave");
    const std::array<uint8_t, 7> expected = {5, 0, 6, 127, 28, 89, 54};
    Check(played.presets[0].envelope == expected, "the early player's envelope starts at nothing");

    // The later player plays the same song from the envelope's first byte (the code at 0x80054E80 in Gex 64).
    const ultralib::sequence_export::n64::LibmusPerformance later =
        PlayEarlySong(MusEarlySong({{.data = {0x85, 120, 0x81, 1, 0x90, 0, 48, 96, 0x80}}}, envelopes), false);

    const std::vector<midi::TrackEvent> later_ons = EventsOfKind(later.performance, midi::EventKind::kNoteOn);
    Check(later_ons.size() == 1 && later_ons[0].key == 60 && later_ons[0].value == 127, "the later player's note");
    Check(later.presets.size() == 1 && later.presets[0].envelope == std::array<uint8_t, 7>{5, 100, 6, 127, 28, 89, 54},
          "the later player's envelope starts at its first byte");
}

void TestLibmusEarlyZone()
{
    ultralib::sequence_export::n64::LibmusSettings settings;
    settings.early_form = true;
    settings.early_player = true;
    settings.song_volume = 128;
    settings.master_volume = 0x6FFF;

    // The envelope's first byte, its time unit, doesn't count: the attack takes 6 ticks and the release 54.
    const ultralib::sequence_export::n64::LibmusPresetKey key = {.wave = 0, .envelope = {5, 0, 6, 127, 28, 89, 54}};
    const Zone zone = ultralib::sequence_export::n64::LibmusZone(key, -48.0, settings, 96.0);
    Check(Near(zone.delay_ms + zone.attack_ms, 6.0 * 1000.0 / 96.0, 1e-6), "the early attack");
    Check(zone.exclusive_class == 0, "a note's release goes on beside the next note");
    Check(Near(zone.release_ms, 2.0 * std::numbers::ln10 * 54.0 * 1000.0 / 96.0, 1e-6), "the early release");

    // An attack of one tick starts at the peak.
    const ultralib::sequence_export::n64::LibmusPresetKey sudden = {.wave = 0, .envelope = {1, 0, 1, 127, 255, 127, 1}};
    Check(Near(ultralib::sequence_export::n64::LibmusZone(sudden, -48.0, settings, 96.0).attack_ms, 0.0, 1e-6),
          "no attack");

    // The later player counts the same envelope's times in units of 5 ticks (the speed command at 0x80053048 and
    // 0x80054F48 in Gex 64), and a new note cuts the last one (0x80054988).
    settings.early_player = false;
    const Zone later = ultralib::sequence_export::n64::LibmusZone(key, -48.0, settings, 96.0);
    Check(Near(later.delay_ms + later.attack_ms, 5.0 * 6.0 * 1000.0 / 96.0, 1e-6), "the later player's attack");
    Check(later.exclusive_class == 1, "the later player's note cuts the last");
    Check(Near(later.release_ms, 2.0 * std::numbers::ln10 * 5.0 * 54.0 * 1000.0 / 96.0, 1e-6),
          "the later player's release");

    // The later player counts a decay from `attack` ticks into the note. An attack of 20 units of 4 ticks ends at tick
    // 80, after the decay of 3 units has run out at tick 32. The envelope then steps from the peak to the sustain
    // (0x80054FA8).
    const ultralib::sequence_export::n64::LibmusPresetKey slow = {.wave = 0, .envelope = {4, 0, 20, 127, 3, 64, 10}};
    const Zone stepped = ultralib::sequence_export::n64::LibmusZone(slow, -48.0, settings, 96.0);
    Check(Near(stepped.delay_ms + stepped.attack_ms, 80.0 * 1000.0 / 96.0, 1e-6), "a slow attack's length");
    Check(Near(stepped.sustain_db, -40.0 * std::log10(64.0 / 127.0), 1e-6), "the sustain after a slow attack");
}

void TestLibmusClippedEnvelope()
{
    // A voice's volume stops at 32767 while volume * envelope * velocity * song volume is 2^28 or more (the code at
    // 0x80054BD4 in Gex 64). At a volume of 127, a velocity of 127 and a song volume of 160, that is from an envelope
    // value of 2^28 / (127 * 127 * 160): 104 and 4 of 256. The note keeps its velocity, and its preset says where the
    // envelope stops. A velocity of 100 reaches the top at 132, above the envelope's 127: that preset's envelope is
    // whole.
    ultralib::sequence_export::n64::LibmusSettings settings;
    settings.early_form = true;
    settings.clipped_envelopes = true;
    settings.song_volume = 160;
    settings.master_volume = 0x3FFF;
    const std::vector<uint8_t> envelopes = {1, 0, 10, 127, 100, 27, 20};
    const std::vector<uint8_t> song = MusEarlySong({{.data = {0x90, 0, 48, 96, 0x9B, 100, 48, 96, 0x80}}}, envelopes);

    const ultralib::sequence_export::n64::LibmusPerformance played =
        ultralib::sequence_export::n64::PerformLibmus(song, kMusDetunes, settings, {});

    const std::vector<midi::TrackEvent> ons = EventsOfKind(played.performance, midi::EventKind::kNoteOn);
    Check(ons.size() == 2 && ons[0].value == 127 && ons[1].value == 100, "a clipped note keeps its velocity");
    Check(played.presets.size() == 2 && played.presets[0].top == 104 * 256 + 4 && played.presets[1].top == 0,
          "where a note's envelope stops");
    Check(played.presets[0].envelope == played.presets[1].envelope, "both presets have the envelope's bytes");

    // The clipped zone rises along the envelope's line to the stop, 10 ticks * 104.016 / 127 at 96 ticks a second.
    // It stays there until the decay's line comes down to it, 10 + 100 * (127 - 104.016) / (127 - 27) ticks into the
    // note. Its top stands at the stop's level, squared by the mixer.
    const double stop = (104.0 + 4.0 / 256.0) / 127.0;
    const double rise_ticks = 10.0 * stop;
    const double fall_ticks = 10.0 + 100.0 * (127.0 - 127.0 * stop) / (127.0 - 27.0);
    const Zone zone = ultralib::sequence_export::n64::LibmusZone(played.presets[0], -48.0, settings, 96.0);
    const Zone whole = ultralib::sequence_export::n64::LibmusZone(played.presets[1], -48.0, settings, 96.0);
    const double full = 127.0 * 127.0 * 127.0 * 160.0 / 8192.0 / 32768.0 * (0x3FFF / 32768.0);
    Check(Near(zone.delay_ms + zone.attack_ms, rise_ticks * 1000.0 / 96.0, 1e-6), "the clipped rise");
    Check(Near(zone.hold_ms, (fall_ticks - rise_ticks) * 1000.0 / 96.0, 1e-6), "the stretch at the stop");
    Check(Near(zone.gain, stop * stop * full * full, 1e-9), "the clipped top");
    Check(Near(zone.sustain_db, -20.0 * std::log10((27.0 / 127.0 / stop) * (27.0 / 127.0 / stop)), 1e-6),
          "the sustain under the clipped top");
    Check(Near(whole.hold_ms, 0.0, 1e-6) && Near(whole.gain, full * full, 1e-9), "the whole envelope's zone");
    Check(Near(whole.delay_ms + whole.attack_ms, 10.0 * 1000.0 / 96.0, 1e-6), "the whole rise");

    // Without clipped envelopes, the first note takes the velocity that reaches the top at the envelope's peak.
    settings.clipped_envelopes = false;
    const ultralib::sequence_export::n64::LibmusPerformance held =
        ultralib::sequence_export::n64::PerformLibmus(song, kMusDetunes, settings, {});

    const std::vector<midi::TrackEvent> held_ons = EventsOfKind(held.performance, midi::EventKind::kNoteOn);
    Check(held_ons.size() == 2 && held_ons[0].value == 104 && held_ons[1].value == 100, "a held velocity");
    Check(held.presets.size() == 1 && held.presets[0].top == 0, "one preset without clipped envelopes");
}

void TestLibmusClippedEnvelopeTiming()
{
    // The game's frame reads the volume list after a note's start and before it sets the voice's volume (the code at
    // 0x800545AC, 0x800545F4 and 0x80054708 in Gex 64). The list's 127 at the note's tick replaces the command's 64,
    // and the note reaches the stop of TestLibmusClippedEnvelope at 104 and 4 of 256. At 64 it wouldn't.
    ultralib::sequence_export::n64::LibmusSettings settings;
    settings.early_form = true;
    settings.clipped_envelopes = true;
    settings.song_volume = 160;
    settings.master_volume = 0x3FFF;
    const std::vector<uint8_t> envelopes = {1, 0, 10, 127, 100, 27, 20};
    const std::vector<uint8_t> song =
        MusEarlySong({{.data = {0xA6, 64, 0x90, 0, 48, 96, 0x80}, .volume = {0xFF, 0x7F}}}, envelopes);

    const ultralib::sequence_export::n64::LibmusPerformance played =
        ultralib::sequence_export::n64::PerformLibmus(song, kMusDetunes, settings, {});

    Check(played.presets.size() == 1 && played.presets[0].top == 104 * 256 + 4, "the stop at the tick's volume");

    // An envelope of speed 8 peaks at tick 80 and falls to 100 at tick 2050. The stop at 104.016 holds from tick 65.5
    // to tick 1757. The fall under the stop is too slow for a SoundFont, and the zone holds one level. A typical note
    // of 2 seconds still stands at the stop. The zone therefore holds the stop.
    const ultralib::sequence_export::n64::LibmusPresetKey slow = {
        .wave = 0, .envelope = {8, 0, 10, 127, 255, 100, 20}, .top = 104 * 256 + 4};
    const Zone zone = ultralib::sequence_export::n64::LibmusZone(slow, -48.0, settings, 96.0);
    const double stop = (104.0 + 4.0 / 256.0) / 127.0;
    const double full = 127.0 * 127.0 * 127.0 * 160.0 / 8192.0 / 32768.0 * (0x3FFF / 32768.0);
    Check(Near(zone.decay_ms, 0.0, 1e-6) && Near(zone.gain, stop * stop * full * full, 1e-9),
          "a slow fall holds the stop");
}

void TestLibmusEarlyBendRange()
{
    // The song's range of 0 takes the first pass's bend. The game's code at 0x80038790 sets the range back to 2/64 at
    // each new pass. A bend list's value of 80 stands 16 steps above the middle.
    const std::vector<uint8_t> data = {0xA8, 0, 0x95, 0xFF, 48, 96, 0x96};
    const std::vector<uint8_t> other = {0x95, 0xFF, 48, 0x81, 0x40, 0x96};
    const std::vector<uint8_t> bend = {0xD0, 0x83, 0x00};
    const ultralib::sequence_export::n64::LibmusPerformance played =
        PlayEarlySong(MusEarlySong({{.data = data, .bend = bend}, {.data = other}}));
    Check(Near(PitchAt(played.performance, 95), 0.0, 1e-6), "the first pass has no bend");
    Check(Near(PitchAt(played.performance, 96), 16.0 / 32.0, 1e-6), "the second pass bends");
}

void TestLibmusLaterBendRange()
{
    // Each pass ends with a range of 0. The later player keeps that range at the next pass (the code at 0x80053544 in
    // Gex 64), and the second pass plays no bend. A bend list's value of 80 stands 16 steps above the middle: half a
    // semitone at the first range of 2/64.
    const std::vector<uint8_t> song =
        MusEarlySong({{.data = {0x95, 0xFF, 48, 96, 0xA8, 0, 0x96}, .bend = {0xD0, 0x83, 0x00}}});
    ultralib::sequence_export::n64::LibmusSettings settings;
    settings.early_form = true;
    settings.loop_on_later_passes = true;

    const ultralib::sequence_export::n64::LibmusPerformance later =
        ultralib::sequence_export::n64::PerformLibmus(song, kMusDetunes, settings, {});

    Check(Near(PitchAt(later.performance, 95), 0.5, 1e-6), "the first pass bends");
    Check(Near(PitchAt(later.performance, 96), 0.0, 1e-6), "the second pass has no bend");

    // Every later pass repeats the second. The main loop starts there.
    Check(later.performance.loop && later.performance.loop->start == 96 && later.performance.loop->end == 192,
          "the loop follows the second pass");

    // The early player sets the range back at each pass. Every pass plays as the first, and so does the loop.
    settings.early_player = true;
    const ultralib::sequence_export::n64::LibmusPerformance early =
        ultralib::sequence_export::n64::PerformLibmus(song, kMusDetunes, settings, {});

    Check(Near(PitchAt(early.performance, 95), 0.5, 1e-6) && Near(PitchAt(early.performance, 150), 0.5, 1e-6),
          "every pass bends");
    Check(early.performance.loop && early.performance.loop->start == 0 && early.performance.loop->end == 96,
          "the loop follows the first pass");

    // Without `loop_on_later_passes`, the main loop plays the first pass.
    settings.early_player = false;
    settings.loop_on_later_passes = false;
    const ultralib::sequence_export::n64::LibmusPerformance first =
        ultralib::sequence_export::n64::PerformLibmus(song, kMusDetunes, settings, {});

    Check(first.performance.loop && first.performance.loop->start == 0 && first.performance.loop->end == 96,
          "the loop plays the first pass");
}

void TestLibmusLaterPassesOfChannels()
{
    ultralib::sequence_export::n64::LibmusSettings settings;
    settings.early_form = true;
    settings.loop_on_later_passes = true;

    // Channel 0 enters its loop at tick 48, with a pass of 144 ticks: a rest of 48 and a note of 96. Its bend list
    // stands at the middle up to tick 98 and then 16 steps above it, and the pass ends on a bent note. Its second pass
    // starts its rest at that pitch. No note plays it. Like the first pass, it then sets the middle again at the note's
    // start. Channel 1 enters at tick 0 with a pass of 32 ticks. Its second pass plays no bend. The channels meet every
    // 288 ticks. The main loop starts at 48, the later of channel 0's first pass and channel 1's second.
    const std::vector<uint8_t> moved = {0x95, 0xFF, 48, 32, 0xA8, 0, 0x96};
    const std::vector<uint8_t> bend = {0xD0, 0x83, 0x00};
    const std::vector<uint8_t> song =
        MusEarlySong({{.data = {48, 48, 0x95, 0xFF, 96, 48, 48, 96, 0x96}, .bend = {0xC0, 96, 0xD0, 0x83, 0x00}},
                      {.data = moved, .bend = bend}});

    const ultralib::sequence_export::n64::LibmusPerformance played =
        ultralib::sequence_export::n64::PerformLibmus(song, kMusDetunes, settings, {});

    Check(played.performance.loop && played.performance.loop->start == 48 && played.performance.loop->end == 336,
          "the main loop starts where every channel repeats");

    // A channel whose second pass can't be read keeps every channel's loop at the first pass. Channel 1 turns drums on
    // at the end of its pass, and its second pass's note reads a drum past the song's end.
    const std::vector<uint8_t> unread = {0x95, 0xFF, 48, 32, 0x9E, 0, 0x96};
    const ultralib::sequence_export::n64::LibmusPerformance kept = ultralib::sequence_export::n64::PerformLibmus(
        MusEarlySong({{.data = moved, .bend = bend}, {.data = unread}}), kMusDetunes, settings, {});

    Check(kept.performance.loop && kept.performance.loop->start == 0 && kept.performance.loop->end == 32,
          "an unread second pass keeps the first");
}

void TestLibmusPortamento()
{
    // A slide of 12 ticks from note 48 up to note 60.
    const std::vector<uint8_t> data = {48, 96, 0x82, 12, 60, 96, 0x80};
    const ultralib::sequence_export::n64::LibmusPerformance played = PlayEarlySong(MusEarlySong({{.data = data}}));
    Check(Near(PitchAt(played.performance, 96), -12.0, 1e-6), "a slide starts at the last note");
    Check(Near(PitchAt(played.performance, 102), -6.0, 1e-6), "a slide's middle");
    Check(Near(PitchAt(played.performance, 108), 0.0, 1e-6), "a slide ends at the note");
}

void TestLibmusVibrato()
{
    // A period of 20 ticks and a depth of a semitone, after 10 ticks. The early form's count wraps before then, and the
    // pitch holds one value.
    const std::vector<uint8_t> data = {0x88, 10, 20, 50, 48, 96, 0x80};
    const ultralib::sequence_export::n64::LibmusPerformance played = PlayEarlySong(MusEarlySong({{.data = data}}));
    Check(Near(PitchAt(played.performance, 3), PitchAt(played.performance, 8), 1e-6), "no vibrato before its delay");
    Check(Near(PitchAt(played.performance, 15), 1.0, 0.1), "the vibrato's top");
    Check(Near(PitchAt(played.performance, 25), -1.0, 0.1), "the vibrato's bottom");
}

void TestLibmusLaterVibrato()
{
    // The later player computes the vibrato only after its delay, and keeps the last value up to there (the code at
    // 0x800551BC in Gex 64). The first note starts at no vibrato. The first note's last tick, 85 ticks after the
    // delay, stands at the top. The second note holds that top up to its delay's end.
    const std::vector<uint8_t> data = {0x88, 10, 20, 50, 48, 96, 48, 96, 0x80};

    const ultralib::sequence_export::n64::LibmusPerformance played =
        PlayEarlySong(MusEarlySong({{.data = data}}), false);

    Check(Near(PitchAt(played.performance, 5), 0.0, 1e-6), "no vibrato before the first delay");
    Check(Near(PitchAt(played.performance, 15), 1.0, 1e-6), "the vibrato's top");
    Check(Near(PitchAt(played.performance, 95), 1.0, 1e-6), "the first note's last value");
    Check(Near(PitchAt(played.performance, 104), 1.0, 1e-6), "the last value holds up to the delay");
    Check(Near(PitchAt(played.performance, 116), 0.0, 1e-6), "the vibrato after the delay");
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("sequence_export_n64_libmus",
                                    []()
                                    {
                                        TestLibmusNote();
                                        TestLibmusVelocity();
                                        TestLibmusDrums();
                                        TestLibmusLists();
                                        TestLibmusLoops();
                                        TestLibmusZone();
                                        TestLibmusSharedTrack();
                                        TestLibmusEarlySong();
                                        TestLibmusEarlyZone();
                                        TestLibmusClippedEnvelope();
                                        TestLibmusClippedEnvelopeTiming();
                                        TestLibmusEarlyBendRange();
                                        TestLibmusLaterBendRange();
                                        TestLibmusLaterPassesOfChannels();
                                        TestLibmusPortamento();
                                        TestLibmusVibrato();
                                        TestLibmusLaterVibrato();
                                        TestUnpackDirectoryFile();
                                        TestLibmusTempoScale();
                                        TestLibmusRepeats();
                                        TestLibmusSongTables();
                                        TestLibmusReverbBase();
                                        TestLibmusSongList();
                                        TestLibmusTablesOutsideImage();
                                        TestLibmusSend();
                                    });
}
