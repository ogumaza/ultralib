// SPDX-License-Identifier: MIT

// The plan of one pass of the ADPCM decoder. The three EAD engines share it. An engine decodes the samples of a voice's
// update in passes. A pass ends where the update's samples end, or at the end of the sample or its loop.

#pragma once

#include <cstdint>

#include "ead_constants.h"

namespace ultralib::n64
{

// One pass: the samples to skip in the first frame and those left in it, the frames to load and the samples to decode,
// the samples after the end of the pass, and whether the pass reaches the loop or the end.
struct EadAdpcmPass
{
    int ignored_first = 0;
    int in_first = 0;
    int frames = 0;
    int to_decode = 0;
    int trailing = 0;
    bool restart = false;
    bool note_finished = false;

    // The samples that the pass adds to the part: those left in the first frame and those decoded, less those after
    // the pass's end.
    int Samples() const
    {
        return to_decode + in_first - trailing;
    }
};

// Plans the pass that decodes `to_process` samples from `position`. `until_end` is the number of samples up to the end
// of the sample or its loop. A pass that reaches that end restarts a sample that `loops` and finishes the note of any
// other sample. A pass with no frames decodes nothing.
inline EadAdpcmPass PlanAdpcmPass(int32_t position, bool restarting, int32_t until_end, int to_process, bool loops)
{
    using ead_constants::kFrameSamples;

    EadAdpcmPass pass;
    pass.ignored_first = position & 0xF;
    if (pass.ignored_first == 0 && !restarting)
    {
        pass.ignored_first = kFrameSamples;
    }

    pass.in_first = kFrameSamples - pass.ignored_first;
    if (to_process < until_end)
    {
        pass.frames = (to_process - pass.in_first + kFrameSamples - 1) / kFrameSamples;
        pass.to_decode = pass.frames * kFrameSamples;
        pass.trailing = pass.in_first + pass.to_decode - to_process;
    }
    else
    {
        pass.to_decode = until_end - pass.in_first;
        pass.trailing = 0;

        if (pass.to_decode <= 0)
        {
            pass.to_decode = 0;
            pass.in_first = until_end;
        }

        pass.frames = (pass.to_decode + kFrameSamples - 1) / kFrameSamples;

        if (loops)
        {
            pass.restart = true;
        }
        else
        {
            pass.note_finished = true;
        }
    }

    if (pass.frames == 0)
    {
        pass.to_decode = 0;
    }

    return pass;
}

// The bytes of a part's samples in DMEM after a pass without the init flag. `written` is the count before the pass. A
// part's first pass without the loop flag keeps the samples that it skips in its first frame.
inline int WrittenAfterPass(bool loop, int written, const EadAdpcmPass& pass)
{
    if (loop || written != 0)
    {
        return pass.Samples() * 2 + written;
    }

    return (pass.ignored_first + pass.Samples()) * 2;
}

} // namespace ultralib::n64
