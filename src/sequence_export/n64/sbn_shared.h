// SPDX-License-Identifier: MIT

// What the SBN performer, its zones and its games share.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "ultralib/n64/sbn.h"

namespace ultralib::sequence_export::n64::sbn
{

// The clock.
inline constexpr uint32_t kNominalRate = 32000;

// The voices.
inline constexpr int kFirstSongDrum = 72;

// The levels.
inline constexpr int kFullLevel = 127;
inline constexpr double kEnvelopeScale = 128.0;
inline constexpr double kVelocityScale = 128.0;

// Pitch.
inline constexpr double kHighestRatio = 1.99996;
inline constexpr int kCentsPerKey = 100;

inline constexpr uint8_t kFirstEnvelopeCommand = 0x80;
inline constexpr uint8_t kRelativeLevel = 0x80;
inline constexpr uint8_t kLevelMask = 0x7F;

inline constexpr int kCentrePan = 64;
inline constexpr int kHighestPan = 127;

// The drum that a key of a drum track plays, or null. Keys past a song's list play its first drum.
inline const ultralib::n64::BgmDrum* DrumOf(const ultralib::n64::BgmFile& file,
                                            std::span<const ultralib::n64::BgmDrum> per, int key)
{
    if (key < kFirstSongDrum)
    {
        return static_cast<std::size_t>(key) < per.size() ? &per[static_cast<std::size_t>(key)] : nullptr;
    }

    if (file.drums.empty())
    {
        return nullptr;
    }

    const auto index = static_cast<std::size_t>(key - kFirstSongDrum);

    return index < file.drums.size() ? &file.drums[index] : &file.drums[0];
}

} // namespace ultralib::sequence_export::n64::sbn
