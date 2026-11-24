// SPDX-License-Identifier: MIT

// The decoding of an engine's waves: VADPCM in either frame size, or 16-bit samples, with the loop that a SoundFont or
// an SFZ player repeats.

#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>

#include "seqlib/pcm.h"

#include "ultralib/n64/bank_file.h"
#include "ultralib/n64/vadpcm.h"
#include "ultralib/sequence_export/n64/instruments.h"

namespace ultralib::sequence_export::n64
{

// The size of a wave's VADPCM frames: 9 bytes, or the 5 of the small frames of Nintendo EAD's later engines.
enum class VadpcmFrames
{
    kStandard,
    kSmall,
};

// Decodes a wave's `bytes`, with `book` for VADPCM or as 16-bit samples without one. A `loop` may be null. A loop with
// a count of 0, or one that doesn't fit the samples, leaves the samples unlooped. A VADPCM wave's repeats start from
// the state that the loop holds, at the frame of the loop's start. Where they differ from the first pass, the samples
// keep one pass more, and the loop repeats that pass.
seqlib::Pcm DecodeWaveSamples(std::span<const uint8_t> bytes, const ultralib::n64::VadpcmBook* book,
                              const ultralib::n64::BankLoop* loop, VadpcmFrames frames = VadpcmFrames::kStandard);

// The wave of DecodeWaveSamples, named `name`. `rate` plays it at its zone's tuning.
std::shared_ptr<const Wave> DecodeWave(std::span<const uint8_t> bytes, const ultralib::n64::VadpcmBook* book,
                                       const ultralib::n64::BankLoop* loop, std::string name, uint32_t rate);

} // namespace ultralib::sequence_export::n64
