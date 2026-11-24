// SPDX-License-Identifier: MIT

// The constants that the source files of the model of Hey You, Pikachu!'s player share. ambrella_player.cpp contains
// the player and its voices, and ambrella_player_reader.cpp the reader of its sequences.

#pragma once

#include <cstdint>

namespace ultralib::n64::ambrella_player_constants
{

inline constexpr uint8_t kStatusNoteOff = 0x80;
inline constexpr uint8_t kStatusNoteOn = 0x90;
inline constexpr uint8_t kStatusController = 0xB0;
inline constexpr uint8_t kStatusProgram = 0xC0;
inline constexpr uint8_t kStatusBend = 0xE0;
inline constexpr uint8_t kStatusSystem = 0xF0;
inline constexpr uint8_t kStatusSystemEnd = 0xF7;
inline constexpr uint8_t kStatusMeta = 0xFF;
inline constexpr uint8_t kMetaEnd = 0x2F;
inline constexpr uint8_t kMetaTempo = 0x51;

inline constexpr uint8_t kControllerModulation = 1;
inline constexpr uint8_t kControllerPortamentoTime = 5;
inline constexpr uint8_t kControllerDataHigh = 6;
inline constexpr uint8_t kControllerVolume = 7;
inline constexpr uint8_t kControllerPan = 10;
inline constexpr uint8_t kControllerExpression = 11;
inline constexpr uint8_t kControllerDataLow = 38;
inline constexpr uint8_t kControllerPanLow = 42;
inline constexpr uint8_t kControllerSustain = 64;
inline constexpr uint8_t kControllerPortamento = 65;
inline constexpr uint8_t kControllerPortamentoKey = 84;
inline constexpr uint8_t kControllerFxMix = 91;
inline constexpr uint8_t kControllerParameterLow = 100;
inline constexpr uint8_t kControllerParameterHigh = 101;
inline constexpr uint8_t kControllerLoopStart = 102;
inline constexpr uint8_t kControllerLoopEnd = 103;
inline constexpr uint8_t kControllerSoundOff = 120;
inline constexpr uint8_t kControllerReset = 121;
inline constexpr uint8_t kControllerNotesOff = 123;

inline constexpr uint8_t kPedalDown = 64;
inline constexpr double kMicroseconds = 0.000001;
inline constexpr uint8_t kMaximum7Bit = 127;
inline constexpr double kFadeTimeUnits = 16.0;

// The system message that the reader makes for a fade: F0 7F 7F 04 03 time level F7.
inline constexpr uint8_t kUniversalRealTime = 0x7F;
inline constexpr uint8_t kDeviceControl = 0x04;
inline constexpr uint8_t kMasterVolumeMessage = 0x01;
inline constexpr uint8_t kFadeMessage = 0x03;

} // namespace ultralib::n64::ambrella_player_constants
