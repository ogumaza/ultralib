// SPDX-License-Identifier: MIT

// The constants and the arithmetic that the source files of libultra's synthesizer share. al_synth.cpp contains the
// voices and their parameters. al_synth_standard.cpp builds the audio lists for the standard microcode, and
// al_synth_compact.cpp the lists for the compact microcode.

#pragma once

#include <cstdint>

namespace ultralib::n64::al_synth_constants
{

inline constexpr int kAdpcmFrameBytes = 9;
inline constexpr int kAdpcmFrameSamples = 16;
inline constexpr int kAdpcmFrameShift = 4;
inline constexpr double kMaxRatio = 1.99996;
inline constexpr int32_t kUnityPitch = 0x8000;
inline constexpr uint32_t kForever = 0xFFFFFFFF;

// A run of samples in the effect's delay line. A run past the end of the line goes on at its start.
struct FxRun
{
    uint32_t address = 0; // the run's first byte in memory
    int64_t before = 0;   // the samples up to the end of the line, or the whole run when it fits
    int64_t after = 0;    // the samples from the start of the line, or 0 when the run fits
};

// The run of `count` samples from `position` in a line of `length` samples at `base`. A negative position counts back
// from the end of the line.
inline FxRun SplitFxRun(uint32_t base, uint32_t length, int64_t position, int count)
{
    if (position < 0)
    {
        position += length;
    }

    const int64_t end = position + count;
    const auto line = static_cast<int64_t>(length);
    FxRun run;
    run.address = static_cast<uint32_t>(base + position * 2);
    run.before = end > line ? line - position : count;
    run.after = end > line ? end - line : 0;

    return run;
}

} // namespace ultralib::n64::al_synth_constants
