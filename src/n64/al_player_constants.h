// SPDX-License-Identifier: MIT

// The MIDI status bytes and the helpers that the sources of libultra's sequence players share. al_player.cpp contains
// the queue of events and the oscillators, al_player_midi.cpp the channel messages, al_player_voices.cpp the voices and
// al_player_sequences.cpp the readers of the sequences.

#pragma once

#include <cstdint>

namespace ultralib::n64::al_player_constants
{

inline constexpr uint8_t kStatusNoteOff = 0x80;
inline constexpr uint8_t kStatusNoteOn = 0x90;
inline constexpr uint8_t kStatusPolyPressure = 0xA0;
inline constexpr uint8_t kStatusController = 0xB0;
inline constexpr uint8_t kStatusProgram = 0xC0;
inline constexpr uint8_t kStatusChannelPressure = 0xD0;
inline constexpr uint8_t kStatusPitchBend = 0xE0;

// The highest volume of a channel or a player.
inline constexpr uint8_t kVolumeFull = 127;

// An effect mix with the player's offset added. The sum wraps at 8 bits, and a result over 127 becomes 127.
inline uint8_t OffsetFxMix(uint8_t mix, int8_t offset)
{
    const auto sum = static_cast<uint8_t>(mix + offset);

    return sum > 127 ? static_cast<uint8_t>(127) : sum;
}

} // namespace ultralib::n64::al_player_constants
