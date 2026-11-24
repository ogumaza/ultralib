// SPDX-License-Identifier: MIT

// Unit tests for the conversion of the Nintendo 64's sound data that every engine shares: the input, the labels and
// the resampler's low-pass. The tests build small sound files in memory. Their expected values follow from the
// arithmetic of the engines, worked out by hand.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <numbers>
#include <string>
#include <utility>
#include <vector>

#include "seqlib/sf2/reader.h"
#include "seqlib/wav.h"
#include "xsflib/binary.h"
#include "xsflib/testing/check.h"
#include "xsflib/testing/near.h"
#include "xsflib/testing/zip_archive.h"

#include "sequence_export_n64_testing.h"
#include "ultralib/n64/audio_rsp.h"
#include "ultralib/sequence_export/n64/ead.h"
#include "ultralib/sequence_export/n64/games.h"
#include "ultralib/sequence_export/n64/image.h"
#include "ultralib/sequence_export/n64/input.h"
#include "ultralib/sequence_export/n64/libmus.h"
#include "ultralib/sequence_export/n64/libultra.h"
#include "ultralib/sequence_export/n64/resampler_low_pass.h"
#include "ultralib/sequence_export/n64/sbn.h"

namespace
{

namespace n64 = ultralib::n64;
namespace midi = seqlib::midi;
using ultralib::sequence_export::n64::Image;
using ultralib::sequence_export::n64::Zone;
using xsflib::testing::Check;
using xsflib::testing::Near;
using xsflib::testing::StoredEntry;
using xsflib::testing::ZipArchive;

void TestImageOrders()
{
    std::vector<uint8_t> console(0x40, 0);
    console[0] = 0x80;
    console[1] = 0x37;
    console[2] = 0x12;
    console[3] = 0x40;
    const std::string name = "A GAME";
    std::ranges::copy(name, console.begin() + 0x20);
    std::fill(console.begin() + 0x26, console.begin() + 0x34, ' ');

    std::vector<uint8_t> halfwords = console;
    for (std::size_t i = 0; i + 1 < halfwords.size(); i += 2)
    {
        std::swap(halfwords[i], halfwords[i + 1]);
    }

    std::vector<uint8_t> words = console;
    for (std::size_t i = 0; i + 3 < words.size(); i += 4)
    {
        std::swap(words[i], words[i + 3]);
        std::swap(words[i + 1], words[i + 2]);
    }

    const Image first(console);
    Check(first.HeaderName() == name, "the header's name loses its trailing spaces");
    Check(Image(halfwords).Sha1() == first.Sha1(), "a .v64 image comes back in the console's order");
    Check(Image(words).Sha1() == first.Sha1(), "a .n64 image comes back in the console's order");

    bool threw = false;
    try
    {
        const Image image(std::vector<uint8_t>(0x40, 0x55));
        Check(image.Sha1().empty(), "bytes that start no image have no hash");
    }
    catch (const xsflib::FormatError&)
    {
        threw = true;
    }

    Check(threw, "bytes that start no image throw");
}

// The message that Input::Open throws for `zip`, written to a file.
std::string OpenMessage(const std::vector<uint8_t>& zip)
{
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "ultralib_sequence_export_n64_test.zip";
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(zip.data()), std::streamsize(zip.size()));

    std::string message;
    try
    {
        ultralib::sequence_export::n64::Input::Open(path, "test");
    }
    catch (const std::exception& e)
    {
        message = e.what();
    }

    std::error_code error;
    std::filesystem::remove(path, error);

    return message;
}

// A cartridge image in a ZIP archive reads as the image. Images are often kept that way. The image's name in the
// archive ends in .z64, .v64 or .n64, in capitals or not. An archive with two images still reads as a ROM set.
void TestZippedImage()
{
    std::vector<uint8_t> image(0x40, 0);
    image[0] = 0x80;
    image[1] = 0x37;
    image[2] = 0x12;
    image[3] = 0x40;

    const std::string one =
        OpenMessage(ZipArchive({StoredEntry("Game.Z64", image), StoredEntry("readme.txt", {'h', 'i'})}));
    const std::string two = OpenMessage(ZipArchive({StoredEntry("a.z64", image), StoredEntry("b.n64", image)}));

    Check(one == "the converter doesn't know this game's sound engine", "an archive's one image reads: " + one);
    Check(!two.empty() && two != one, "an archive with two images reads as a set: " + two);
}

// A track's label gives the settings that set it apart from the other tracks of its sequence.
void TestTrackLabels()
{
    namespace n64_export = ultralib::sequence_export::n64;
    const std::array kTracks = {
        n64_export::EadTrack{.sequence = 0x05},
        n64_export::EadTrack{.sequence = 0x05, .variation = -128, .silent_channels = 0x0E43},
        n64_export::EadTrack{.sequence = 0x0C, .silent_channels = 0x0100},
        n64_export::EadTrack{.sequence = 0x0C, .silent_channels = 0x0008},
        n64_export::EadTrack{.sequence = 0x2E, .silent_channels = 0x07FF},
        n64_export::EadTrack{.sequence = 0x2E, .silent_channels = 0xFD00, .tempo_change = 17},
        n64_export::EadTrack{.sequence = 0x1D, .ports = n64_export::EadPorts(0, 1, 4, 2)},
        n64_export::EadTrack{.sequence = 0x1D, .call = {.volume = 80}},
        n64_export::EadTrack{.sequence = 0x13, .song_volume = 78},
    };
    const n64_export::EadProfile profile = {.tracks = kTracks};

    std::vector<std::string> labels;
    for (const n64_export::EadTrack& track : kTracks)
    {
        labels.push_back(n64_export::EadTrackLabel(profile, track));
    }

    const std::vector<std::string> kExpected = {
        "Sequence 0x05",
        "Sequence 0x05 (variation -128, without channels 0, 1, 6 and 9 to 11)",
        "Sequence 0x0C (without channel 8)",
        "Sequence 0x0C (without channel 3)",
        "Sequence 0x2E (without channels 0 to 10)",
        "Sequence 0x2E (without channels 8 and 10 to 15, tempo +17)",
        "Sequence 0x1D (port 0 = 1, port 4 = 2)",
        "Sequence 0x1D (volume 80)",
        "Sequence 0x13",
    };
    Check(labels == kExpected, "an EAD track's label gives its settings where its sequence has other tracks");
    Check(n64_export::EadTrackSettings({.sequence = 0x13, .song_volume = 78}) == "song volume 78",
          "the volume that a track sets");
    Check(n64_export::LibultraRateSettings({.song = 5, .rate = 0.798}) == "rate 0.798", "a rate change's label");
    Check(n64_export::LibmusTempoSettings({.song = 0x2A, .scale = 0x8E, .beside = true}) == "tempo 142/128",
          "a tempo change's label");
    Check(n64_export::SbnChangeSettings({.mix = 4, .near = true}) == "mix 4, near", "a mix's label");
    Check(n64_export::SbnChangeSettings({.linked = true}) == "linked", "the linked tracks' label");
    Check(n64_export::SbnChangeSettings({.levels = 0x541B8}) == "track levels at 0x541B8", "a list of levels' label");
}

// No two tracks of a game's profile share a label.
void TestProfileLabelsDiffer()
{
    namespace n64_export = ultralib::sequence_export::n64;
    for (const n64_export::GameProfile& game : n64_export::Profiles())
    {
        std::vector<std::string> labels;
        for (const n64_export::LibultraRateChange& change : game.libultra.rate_changes)
        {
            labels.push_back(n64_export::SequenceLabel(change.song, n64_export::LibultraRateSettings(change)));
        }

        if (game.ead != nullptr)
        {
            for (const n64_export::EadTrack& track : game.ead->tracks)
            {
                labels.push_back(n64_export::EadTrackLabel(*game.ead, track));
            }
        }

        if (game.libmus != nullptr)
        {
            for (const n64_export::LibmusTempoChange& change : game.libmus->tempo_changes)
            {
                labels.push_back(n64_export::SequenceLabel(change.song, n64_export::LibmusTempoSettings(change)));
            }
        }

        if (game.sbn != nullptr)
        {
            for (const n64_export::SbnChange& change : game.sbn->changes)
            {
                labels.push_back(n64_export::SequenceLabel(change.song, n64_export::SbnChangeSettings(change)));
            }
        }

        std::ranges::sort(labels);
        Check(std::ranges::adjacent_find(labels) == labels.end(),
              std::string("no two tracks of the profile ") + game.sha1 + " share a label");
    }
}

// The resampler's mean response at `fraction` of the rate: the root mean square over its 64 filters of each filter's
// magnitude, from the microcode's table.
double ResamplerResponse(double fraction)
{
    const std::array<int16_t, 256>& table = n64::AudioResampleTable();
    double power = 0.0;
    for (std::size_t phase = 0; phase < 64; phase++)
    {
        double re = 0.0;
        double im = 0.0;
        for (std::size_t tap = 0; tap < 4; tap++)
        {
            const double w = 2.0 * std::numbers::pi * fraction * static_cast<double>(tap);
            re += table[phase * 4 + tap] / 32768.0 * std::cos(w);
            im -= table[phase * 4 + tap] / 32768.0 * std::sin(w);
        }

        power += (re * re + im * im) / 64.0;
    }

    return std::sqrt(power);
}

double Db(double amplitude)
{
    return 20.0 * std::log10(amplitude);
}

void TestResamplerLowPassResponse()
{
    const std::array<int16_t, 5>& taps = ultralib::sequence_export::n64::ResamplerLowPass();

    Check(taps[0] + 2 * (taps[1] + taps[2] + taps[3] + taps[4]) == 32768, "the filter passes a constant level at 1.0");
    Check(Near(Db(ResamplerResponse(0.25)), -2.26, 0.01) && Near(Db(ResamplerResponse(0.45)), -6.96, 0.01) &&
              Near(Db(ResamplerResponse(0.5)), -7.43, 0.01),
          "the resampler's mean response");
    for (const double fraction : {0.05, 0.1, 0.2, 0.25, 0.3, 0.35, 0.4, 0.45})
    {
        double response = taps[0];
        for (std::size_t tap = 1; tap < taps.size(); tap++)
        {
            response += 2.0 * taps[tap] * std::cos(2.0 * std::numbers::pi * fraction * static_cast<double>(tap));
        }

        Check(Near(Db(response / 32768.0), Db(ResamplerResponse(fraction)), 0.02),
              "the filter follows the resampler's mean response");
    }
}

void TestResamplerLowPassSine()
{
    // A sine at a quarter of the rate, with no loop. Away from the ends, the filter takes it down by the resampler's
    // mean response there, and the length stays.
    seqlib::Pcm pcm;
    pcm.channels.emplace_back();
    for (int i = 0; i < 4000; i++)
    {
        pcm.channels[0].push_back(
            static_cast<int16_t>(std::lround(16384.0 * std::sin(0.5 * std::numbers::pi * i + 0.3))));
    }

    const seqlib::Pcm out = ultralib::sequence_export::n64::ApplyResamplerLowPass(pcm);

    Check(out.channels.size() == 1 && out.channels[0].size() == 4000 && !out.loop, "the length stays");
    double in_power = 0.0;
    double out_power = 0.0;
    for (std::size_t i = 100; i < 3900; i++)
    {
        in_power += static_cast<double>(pcm.channels[0][i]) * pcm.channels[0][i];
        out_power += static_cast<double>(out.channels[0][i]) * out.channels[0][i];
    }

    Check(Near(10.0 * std::log10(out_power / in_power), Db(ResamplerResponse(0.25)), 0.02), "a sine at a quarter");
}

void TestResamplerLowPassEnds()
{
    // A constant level of 1000 with no loop. Inside, it stays. At each end, the filter reads zeros past the wave.
    const std::array<int16_t, 5>& taps = ultralib::sequence_export::n64::ResamplerLowPass();
    seqlib::Pcm pcm;
    pcm.channels.emplace_back(32, int16_t{1000});

    const seqlib::Pcm out = ultralib::sequence_export::n64::ApplyResamplerLowPass(pcm);

    const std::vector<int16_t>& s = out.channels.at(0);
    Check(s.size() == 32 && std::all_of(s.begin() + 4, s.end() - 4, [](int16_t v) { return v == 1000; }),
          "a constant level stays");
    const int32_t half = taps[0] + taps[1] + taps[2] + taps[3] + taps[4];
    Check(s[0] == (1000 * half + 16384) >> 15 && s[31] == s[0], "zeros before the first sample and after the last");
}

void TestResamplerLowPassLoop()
{
    // An attack of 10 samples and a loop of 7, shorter than the filter. The filtered wave plays as the filtered wave
    // that repeats the loop 20 times: the loop's last samples and its first join as the loop wraps. Only the first
    // pass's first 4 samples of the loop differ. There they read the attack, and in the filtered wave the loop's end.
    seqlib::Pcm pcm;
    pcm.loop = true;
    pcm.loop_start = 10;
    pcm.loop_end = 17;
    pcm.channels.emplace_back();
    for (int i = 0; i < 17; i++)
    {
        pcm.channels[0].push_back(static_cast<int16_t>((i * 7919) % 20001 - 10000));
    }

    seqlib::Pcm unrolled;
    unrolled.channels.emplace_back(pcm.channels[0].begin(), pcm.channels[0].begin() + 10);
    for (int repeat = 0; repeat < 20; repeat++)
    {
        unrolled.channels[0].insert(unrolled.channels[0].end(), pcm.channels[0].begin() + 10, pcm.channels[0].end());
    }

    const seqlib::Pcm out = ultralib::sequence_export::n64::ApplyResamplerLowPass(pcm);
    const seqlib::Pcm played = ultralib::sequence_export::n64::ApplyResamplerLowPass(unrolled);

    Check(out.loop && out.loop_start == 10 && out.loop_end == 17 && out.channels.at(0).size() == 17, "the loop stays");
    bool same = true;
    for (std::size_t i = 0; i < 10 + 7 * 10; i++)
    {
        if (i >= 10 && i < 14)
        {
            continue;
        }

        const std::size_t looped = i < 10 ? i : 10 + (i - 10) % 7;
        same = same && out.channels[0][looped] == played.channels[0][i];
    }

    Check(same, "the attack and the loop play as the repeated loop");
}

void TestResamplerLowPassClips()
{
    // Three samples at the top between pairs at the bottom. The middle one goes past 16 bits, to 34343, and is
    // limited.
    seqlib::Pcm pcm;
    pcm.channels.push_back({-32768, -32768, 32767, 32767, 32767, -32768, -32768});
    std::size_t clipped = 0;

    const seqlib::Pcm out = ultralib::sequence_export::n64::ApplyResamplerLowPass(pcm, &clipped);

    Check(clipped == 1 && out.channels.at(0)[3] == 32767, "a sample past 16 bits is limited");
    Check(out.channels[0][2] == out.channels[0][4] && out.channels[0][2] < 32767, "its neighbours stay inside");
}

void TestResamplerFilterOption()
{
    // A wave whose samples swing at half its rate, where the low-pass takes it down by 7.4 dB. By default, the
    // SoundFont's sample and the SFZ file contain the filtered wave. Without the filter, both contain the wave.
    seqlib::Pcm pcm;
    pcm.channels.emplace_back();
    for (int i = 0; i < 64; i++)
    {
        pcm.channels[0].push_back(static_cast<int16_t>(i % 2 == 0 ? 10000 : -10000));
    }

    const auto build = [&pcm](bool resampler_filter)
    {
        ultralib::sequence_export::n64::Performance performance;
        performance.timebase = 48;
        auto wave = std::make_shared<ultralib::sequence_export::n64::Wave>();
        wave->name = "w";
        wave->rate = 32000;
        wave->pcm = pcm;
        ultralib::sequence_export::n64::Instrument preset;
        preset.name = "p0";
        preset.zones.push_back({.wave = wave});
        performance.presets.push_back(preset);
        performance.tracks[0] = {{.tick = 0, .kind = midi::EventKind::kPreset, .preset = 0},
                                 {.tick = 0, .kind = midi::EventKind::kNoteOn, .key = 60, .value = 100, .note = 1},
                                 {.tick = 48, .kind = midi::EventKind::kNoteOff, .key = 60, .note = 1}};

        return ultralib::sequence_export::n64::BuildFiles(performance, "test", "test", resampler_filter);
    };

    const auto wave_file = [](const ultralib::sequence_export::n64::Conversion& c)
    {
        return c.sfz.samples.size() == 1 ? seqlib::ReadWav(c.sfz.samples[0].wav).pcm.channels.at(0)
                                         : std::vector<int16_t>();
    };

    const ultralib::sequence_export::n64::Conversion filtered = build(true);
    const ultralib::sequence_export::n64::Conversion kept = build(false);

    const std::vector<int16_t> low_passed = ultralib::sequence_export::n64::ApplyResamplerLowPass(pcm).channels[0];
    Check(wave_file(filtered) == low_passed, "by default, the SFZ file holds the filtered wave");
    Check(wave_file(kept) == pcm.channels[0], "without the filter, the SFZ file holds the wave");
    Check(seqlib::sf2::Read(filtered.sf2).SamplePeak(0) < 10000, "by default, the SoundFont's sample is filtered");
    Check(seqlib::sf2::Read(kept.sf2).SamplePeak(0) == 10000, "without the filter, the SoundFont's sample is the wave");
}

void TestLoudZoneThatNoNotePlays()
{
    // Preset 0 plays at full level below velocity 64 and 10.4 dB above it there. A note at velocity 40 plays the first
    // zone only. The SoundFont keeps its level. A note at velocity 100 plays the loud zone, and every zone then
    // takes the cut.
    const auto build = [](uint8_t velocity)
    {
        ultralib::sequence_export::n64::Performance performance;
        performance.timebase = 48;
        ultralib::sequence_export::n64::Instrument preset;
        preset.name = "p0";
        auto wave = std::make_shared<ultralib::sequence_export::n64::Wave>();
        wave->name = "w";
        wave->rate = 32000;
        wave->pcm.channels = {std::vector<int16_t>(64, 1000)};
        ultralib::sequence_export::n64::Zone quiet;
        quiet.wave = wave;
        quiet.high_velocity = 63;
        ultralib::sequence_export::n64::Zone loud;
        loud.wave = wave;
        loud.low_velocity = 64;
        loud.gain = 3.31;
        preset.zones = {quiet, loud};
        performance.presets.push_back(preset);
        performance.tracks[0] = {{.tick = 0, .kind = midi::EventKind::kPreset, .preset = 0},
                                 {.tick = 0, .kind = midi::EventKind::kNoteOn, .key = 60, .value = velocity, .note = 1},
                                 {.tick = 48, .kind = midi::EventKind::kNoteOff, .key = 60, .note = 1}};

        return ultralib::sequence_export::n64::BuildFiles(performance, "test", "test").performance.approximations;
    };

    const auto cut = [](const auto& approximations)
    {
        return std::ranges::any_of(approximations, [](const auto& entry)
                                   { return entry.first.starts_with("instrument volume above full level"); });
    };

    Check(!cut(build(40)), "a loud zone that no note plays cuts nothing");
    Check(cut(build(100)), "a loud zone that a note plays cuts every zone");
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("sequence_export_n64",
                                    []()
                                    {
                                        TestImageOrders();
                                        TestZippedImage();
                                        TestLoudZoneThatNoNotePlays();
                                        TestTrackLabels();
                                        TestProfileLabelsDiffer();
                                        TestResamplerLowPassResponse();
                                        TestResamplerLowPassSine();
                                        TestResamplerLowPassEnds();
                                        TestResamplerLowPassLoop();
                                        TestResamplerLowPassClips();
                                        TestResamplerFilterOption();
                                    });
}
