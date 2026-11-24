// SPDX-License-Identifier: MIT

// What the tests of the conversion of the Nintendo 64's sound data share.

#pragma once

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <vector>

#include "seqlib/midi/performance.h"

#include "ultralib/n64/bank_file.h"
#include "ultralib/n64/midi.h"
#include "ultralib/sequence_export/n64/instruments.h"
#include "ultralib/sequence_export/n64/libultra.h"

namespace ultralib::testing
{

namespace n64 = ultralib::n64;
namespace midi = seqlib::midi;
using ultralib::sequence_export::n64::LibultraPerformance;
using ultralib::sequence_export::n64::LibultraSettings;
using ultralib::sequence_export::n64::LibultraSong;
using ultralib::sequence_export::n64::PerformLibultra;
using ultralib::sequence_export::n64::PerformOptions;

// A bank of one instrument: two sounds on keys 48 to 84, split at velocity 64. The first has the envelope `envelope`.
inline n64::BankFile SmallBank(const n64::BankEnvelope& envelope)
{
    n64::BankFile file;
    file.envelopes.push_back(envelope);
    file.key_maps.push_back({.velocity_min = 0, .velocity_max = 63, .key_min = 48, .key_max = 84, .key_base = 60});
    file.key_maps.push_back({.velocity_min = 64, .velocity_max = 127, .key_min = 48, .key_max = 84, .key_base = 60});
    file.waves.push_back({.base = 0, .length = 9});
    file.sounds.push_back({.envelope = 0, .key_map = 0, .wave = 0, .pan = 64, .volume = 127});
    file.sounds.push_back({.envelope = 0, .key_map = 1, .wave = 0, .pan = 64, .volume = 127});
    file.instruments.push_back({.volume = 100, .pan = 32, .bend_range = 200, .sounds = {0, 1}});
    file.banks.push_back({.instruments = {0}});

    return file;
}

inline constexpr n64::BankEnvelope kHeldEnvelope = {.attack_volume = 127, .decay_volume = 127};

inline n64::MidiEvent NoteOn(uint32_t delta, int key, int velocity, uint32_t duration)
{
    return {.delta = delta,
            .status = 0x90,
            .data1 = static_cast<uint8_t>(key),
            .data2 = static_cast<uint8_t>(velocity),
            .duration = duration};
}

inline n64::MidiEvent Controller(uint32_t delta, int controller, int value)
{
    return {.delta = delta,
            .status = 0xB0,
            .data1 = static_cast<uint8_t>(controller),
            .data2 = static_cast<uint8_t>(value)};
}

inline n64::MidiEvent EndOfTrack(uint32_t delta)
{
    return {.delta = delta, .status = n64::kMidiMeta, .type = n64::kMidiMetaEndOfTrack};
}

inline LibultraPerformance Play(const std::vector<n64::MidiEvent>& track, const n64::BankFile& file,
                                const PerformOptions& options = {}, const LibultraSettings& settings = {})
{
    LibultraSong song;
    song.tracks.push_back(track);

    return PerformLibultra(song, file, file.banks[0], settings, options);
}

inline std::vector<midi::TrackEvent> EventsOfKind(const ultralib::sequence_export::n64::Performance& performance,
                                                  midi::EventKind kind)
{
    std::vector<midi::TrackEvent> events;
    std::ranges::copy_if(performance.tracks[0], std::back_inserter(events),
                         [kind](const midi::TrackEvent& event) { return event.kind == kind; });

    return events;
}

inline std::vector<midi::TrackEvent> AllEventsOfKind(const ultralib::sequence_export::n64::Performance& performance,
                                                     midi::EventKind kind)
{
    std::vector<midi::TrackEvent> events;
    for (const auto& track : performance.tracks)
    {
        std::ranges::copy_if(track, std::back_inserter(events),
                             [kind](const midi::TrackEvent& event) { return event.kind == kind; });
    }

    return events;
}

} // namespace ultralib::testing
