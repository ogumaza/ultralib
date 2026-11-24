// SPDX-License-Identifier: MIT

// What the performer of the N64 Music Tools, its zones and its games share.

#pragma once

namespace ultralib::sequence_export::n64::libmus
{

// The library names a note of 48 C4. In MIDI, C4 is key 60.
inline constexpr int kKeyOfNote = 12;

// The player's volume above which the voice's volume stops.
inline constexpr int kHighestVoiceVolume = 32767;
inline constexpr int kVolumeShift = 13;

// A preset's `top` counts this many steps for each step of an envelope's bytes.
inline constexpr int kTopSteps = 256;

} // namespace ultralib::sequence_export::n64::libmus
