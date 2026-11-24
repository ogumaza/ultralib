// SPDX-License-Identifier: MIT

// The constants that the models of the three EAD engines and their conversion share. Each engine's constants header
// contains the rest.

#pragma once

#include <array>
#include <cstdint>

#include "ultralib/n64/ead_bank.h"

namespace ultralib::n64::ead_constants
{

// The bits of a channel's mute behaviour: what the channel does while its player is muted.
inline constexpr uint8_t kMuteSoften = 1U << 5;
inline constexpr uint8_t kMuteStopNotes = 1U << 6;
inline constexpr uint8_t kMuteStopScript = 1U << 7;

// A voice's priorities. The later engine doesn't use kPriorityMinimum.
inline constexpr uint8_t kPriorityDisabled = 0;
inline constexpr uint8_t kPriorityStopping = 1;
inline constexpr uint8_t kPriorityMinimum = 2;
inline constexpr uint8_t kPriorityDefault = 3;

// The top bit of a portamento's mode in the middle and later engines. It changes how a layer reads the portamento's
// time and how the engine sets its speed.
inline constexpr uint8_t kPortamentoSpecial = 0x80;

// The ticks in a beat. A player's tempo counts ticks a minute.
inline constexpr int kTicksPerBeat = 48;

// A channel's vibrato rate after a reset, in all three engines.
inline constexpr uint16_t kChannelVibratoRate = 0x800;

// The default envelope of the early and middle engines: up to 32000 over 4 updates, a hold, and a stay.
inline constexpr std::array<EadEnvelope::Point, 3> kDefaultEnvelopePoints = {
    {{4, 32000}, {1000, 32000}, {kEnvelopeHang, 0}}};

// The samples in a frame of ADPCM.
inline constexpr int kFrameSamples = 16;

// The samples of each of the four harmonics that a synthetic wave of the middle and later engines contains
// (ead_later_tables.h).
inline constexpr int kWaveSamples = 64;

} // namespace ultralib::n64::ead_constants

namespace ultralib::n64::ead_standard_dmem
{

// The places in DMEM that the early engine's commands name, and the middle engine's commands for the standard
// microcode.
inline constexpr uint32_t kTemp = 0x000;
inline constexpr uint32_t kResampled = 0x020;
inline constexpr uint32_t kResampled2 = 0x160;
inline constexpr uint32_t kDecoded = 0x180;
inline constexpr uint32_t kPanTemp = 0x200;
inline constexpr uint32_t kStrongDry = 0x200;
inline constexpr uint32_t kStrongWet = 0x340;
inline constexpr uint32_t kCompressed = 0x3F0;
inline constexpr uint32_t kLeft = 0x4C0;
inline constexpr uint32_t kRight = 0x600;
inline constexpr uint32_t kWetLeft = 0x740;
inline constexpr uint32_t kWetRight = 0x880;

// The bytes of an update's buffer of one channel and of two channels.
inline constexpr uint32_t kOneChannel = 0x140;
inline constexpr uint32_t kTwoChannels = 0x280;

} // namespace ultralib::n64::ead_standard_dmem
