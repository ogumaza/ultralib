// SPDX-License-Identifier: MIT

// The constants that the source files of the early EAD model share. ead_early.cpp contains the sound files and the
// players, ead_early_scripts.cpp the scripts, ead_early_voices.cpp the voices and ead_early_synthesis.cpp the audio
// lists. ead_constants.h contains the constants that the three engines share.

#pragma once

#include <cstdint>

namespace ultralib::n64::ead_early_constants
{

// A player's fade states.
inline constexpr uint8_t kStateFadeOut = 1;
inline constexpr uint8_t kStateFadeIn = 2;
inline constexpr uint8_t kStateFadeTo = 3;
inline constexpr uint8_t kStateFadeStay = 4;

// The pool of voices that every player and channel can take.
inline constexpr int kGlobalPool = 0;

} // namespace ultralib::n64::ead_early_constants
