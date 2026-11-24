// SPDX-License-Identifier: MIT

// The constants that the source files of the SBN engine model share. sbn_player.cpp contains the players and their
// compositions, sbn_player_tracks.cpp the tracks' commands and notes, sbn_player_voices.cpp the voices and their
// envelopes, and sbn_player_synthesis.cpp the synthesis driver and the audio lists.

#pragma once

#include <cstdint>

#include "ultralib/n64/sbn_player.h"

namespace ultralib::n64::sbn_player_constants
{

inline constexpr int kChunk = SbnPlayer::kChunkSamples;
inline constexpr int kChunkBytes = kChunk * 2;
inline constexpr int kFxLength = 14 * kChunk;

// The channel that the delay of a bus acts on.
inline constexpr uint8_t kDelayNone = 0;
inline constexpr uint8_t kDelayLeft = 1;
inline constexpr uint8_t kDelayRight = 2;

// The time of a frame in microseconds, and the scales of a player's update step and tempo.
inline constexpr int kFrameMicroseconds = 5750;
inline constexpr int kUpdateScale = 1000;
inline constexpr int kTempoScale = 100;

// The command that ends an envelope.
inline constexpr uint8_t kEndCommand = 0xFF;

// The flags of a voice: what the synthesis driver has to take from the voice, and the state of the voice's envelope.
inline constexpr uint8_t kSyncAll = 1U << 0;
inline constexpr uint8_t kSyncParams = 1U << 1;
inline constexpr uint8_t kSyncPitch = 1U << 2;
inline constexpr uint8_t kSyncPanMix = 1U << 3;
inline constexpr uint8_t kEnvHandledChange = 1U << 0;
inline constexpr uint8_t kEnvKeyReleased = 1U << 1;
inline constexpr uint8_t kEnvReleasing = 1U << 2;
inline constexpr uint8_t kEnvVolumeChanged = 1U << 3;

} // namespace ultralib::n64::sbn_player_constants
