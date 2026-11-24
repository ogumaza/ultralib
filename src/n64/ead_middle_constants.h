// SPDX-License-Identifier: MIT

// The constants that the source files of the middle EAD model share. ead_middle.cpp contains the players,
// ead_middle_scripts.cpp the scripts, ead_middle_voices.cpp the voices and ead_middle_synthesis.cpp the audio lists.
// ead_constants.h contains the constants that the three engines share, and the places of the lists for the standard
// microcode.

#pragma once

#include <cstdint>

namespace ultralib::n64::ead_middle_constants
{

// The bytes of an update's buffer of one channel and of two channels, in the lists for EAD's microcode.
inline constexpr uint32_t kOneChannel = 0x180;
inline constexpr uint32_t kTwoChannels = 0x300;

} // namespace ultralib::n64::ead_middle_constants
