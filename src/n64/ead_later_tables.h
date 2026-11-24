// SPDX-License-Identifier: MIT

#pragma once

#include <array>
#include <cstdint>

namespace ultralib::n64::ead_later_tables
{

// The synthetic waves, 8 of 256 samples: each contains 64, 32, 16 and 8 samples to the period, its harmonics.
extern const std::array<int16_t, 2048> kWaves;

// The frequency scale of a bend across two octaves, with the value 128 at 1.
extern const std::array<float, 256> kBendOctave;

// The frequency scale of a fine bend, across four semitones.
extern const std::array<float, 256> kBendFine;

// The frequency scale of each note.
extern const std::array<float, 128> kPitchFrequencies;

// The delay in bytes of a side at each halved pan, for a channel with stereo effects in headset mode.
extern const std::array<uint16_t, 64> kHaasDelays;

// A book that a channel's book offset can choose.
extern const std::array<int16_t, 32> kNoteBook;

// The low-pass filters' taps, 8 for each cutoff. The first is the identity.
extern const std::array<int16_t, 128> kLowPass;

// The high-pass filters' taps, 8 for each cutoff from 1.
extern const std::array<int16_t, 120> kHighPass;

} // namespace ultralib::n64::ead_later_tables
