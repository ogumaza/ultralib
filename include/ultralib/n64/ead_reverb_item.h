// SPDX-License-Identifier: MIT

// The place of one update's samples in a reverb's ring buffers, in the middle and later EAD engines.

#pragma once

#include <cstdint>

namespace ultralib::n64
{

// The samples of an update in the ring buffers: `length_a` bytes from sample `start` up to the buffers' end, then
// `length_b` bytes from their start. A downsampled reverb has the microcode save the update's samples at
// `downsample_left` and `downsample_right` in main memory. The CPU thins them into the ring buffers two frames later.
struct EadReverbItem
{
    uint32_t downsample_left = 0;
    uint32_t downsample_right = 0;
    int32_t start = 0;
    int16_t length_a = 0;
    int16_t length_b = 0;
};

} // namespace ultralib::n64
