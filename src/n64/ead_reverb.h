// SPDX-License-Identifier: MIT

// The ring buffers of the reverbs that the middle and later EAD engines share: the place of each update's samples in a
// ring buffer, the CPU's thinning of a downsampled reverb's samples, and the transfers of an update's delayed samples
// between a ring buffer and DMEM. The engines' Reverb types have the same fields for these.

#pragma once

#include <cstdint>

#include "ntdlib/rounding.h"

#include "ead_lists.h"
#include "ultralib/n64/audio_memory.h"
#include "ultralib/n64/ead_reverb_item.h"

namespace ultralib::n64::ead_reverb
{

// The places in DMEM of a reverb's delayed samples: the left side at `wet` and the right side one channel after it.
// A downsampled reverb loads its samples to `temp` first.
struct Places
{
    uint32_t wet = 0;
    uint32_t temp = 0;
    uint32_t one_channel = 0;
};

// Sets `item` to `samples` samples of a ring buffer of `buffer_samples` from `start`: the bytes up to the buffer's end
// in `length_a`, and the bytes after its wrap in `length_b`. Returns the samples after the wrap, or a negative number
// when the item doesn't wrap.
inline int SplitItem(EadReverbItem& item, int start, int samples, int buffer_samples)
{
    const int extra = samples + start - buffer_samples;
    item.start = start;
    if (extra < 0)
    {
        item.length_a = static_cast<int16_t>(samples * 2);
        item.length_b = 0;
    }
    else
    {
        item.length_a = static_cast<int16_t>((samples - extra) * 2);
        item.length_b = static_cast<int16_t>(extra * 2);
    }

    return extra;
}

// Places the samples of an update of `length` output samples at the next position of a reverb's ring buffers, and
// moves the position on.
template <typename Reverb>
void PlaceItem(Reverb& reverb, EadReverbItem& item, int length)
{
    const int samples = length / reverb.downsample;
    const int extra = SplitItem(item, reverb.next_position, samples, reverb.buffer_samples);
    reverb.next_position = extra < 0 ? reverb.next_position + samples : extra;
}

// Thins the samples that the microcode saved in an item's slot two frames ago into the ring buffers of a downsampled
// reverb. The CPU does this: it keeps every `downsample`th sample of each side.
template <typename Reverb>
void ThinSamples(AudioMemory& memory, const Reverb& reverb, const EadReverbItem& item)
{
    int from = 0;
    const auto copy = [&](int count, int ring_start)
    {
        for (int i = 0; i < count; i++)
        {
            const uint32_t at = static_cast<uint32_t>(from) * 2;
            const auto place = static_cast<uint32_t>(ring_start + i) * 2;
            ead_lists::WriteBe16(memory, reverb.left + place, ead_lists::ReadBe16(memory, item.downsample_left + at));
            ead_lists::WriteBe16(memory, reverb.right + place, ead_lists::ReadBe16(memory, item.downsample_right + at));
            from += reverb.downsample;
        }
    };

    copy(item.length_a / 2, item.start);
    copy(item.length_b / 2, 0);
}

// Calls `transfer(dmem, address, size)` for both sides of the ring buffers from sample `offset`: the left side at
// `dmem` and the right side one channel after it.
template <typename Reverb, typename Transfer>
void TransferSides(const Reverb& reverb, uint32_t dmem, int offset, uint32_t size, uint32_t one_channel,
                   Transfer transfer)
{
    const auto bytes = static_cast<uint32_t>(offset) * 2;
    transfer(dmem, reverb.left + bytes, size);
    transfer(dmem + one_channel, reverb.right + bytes, size);
}

// Calls `transfer(dmem, address, size)` for both sides of both parts of an item: the part up to the ring buffer's end
// at `dmem`, then the part after its wrap.
template <typename Reverb, typename Transfer>
void TransferParts(const Reverb& reverb, const EadReverbItem& item, uint32_t dmem, uint32_t one_channel,
                   Transfer transfer)
{
    TransferSides(reverb, dmem, item.start, static_cast<uint32_t>(item.length_a), one_channel, transfer);
    if (item.length_b != 0)
    {
        TransferSides(reverb, dmem + static_cast<uint32_t>(item.length_a), 0, static_cast<uint32_t>(item.length_b),
                      one_channel, transfer);
    }
}

// Loads an item's delayed samples to `places.wet`. A downsampled reverb loads whole blocks of 16 bytes around both
// parts to `places.temp`, then resamples each side up to the output rate. `load(dmem, address, size)` appends a load.
// `resample(in, out, size, side)` appends the resampling of side 0 (left) or 1 (right).
template <typename Reverb, typename Load, typename ResampleSide>
void LoadSamples(const Reverb& reverb, const EadReverbItem& item, int length, const Places& places, Load load,
                 ResampleSide resample)
{
    if (reverb.downsample == 1)
    {
        TransferParts(reverb, item, places.wet, places.one_channel, load);

        return;
    }

    const int start_pad = (item.start % 8) * 2;
    const int padded = ntdlib::AlignUp(start_pad + item.length_a, 16);
    TransferSides(reverb, places.temp, item.start - start_pad / 2, places.one_channel, places.one_channel, load);
    if (item.length_b != 0)
    {
        TransferSides(reverb, places.temp + static_cast<uint32_t>(padded), 0,
                      places.one_channel - static_cast<uint32_t>(padded), places.one_channel, load);
    }

    const auto pad = static_cast<uint32_t>(start_pad);
    const auto size = static_cast<uint32_t>(length) * 2;
    resample(places.temp + pad, places.wet, size, 0);
    resample(places.temp + places.one_channel + pad, places.wet + places.one_channel, size, 1);
}

// Advances the reverbs of an engine to the next frame: each one counts down the frames that it ignores and swaps its
// slots of items.
template <typename Reverbs>
void EndFrame(Reverbs& reverbs)
{
    for (auto& reverb : reverbs)
    {
        if (reverb.frames_to_ignore != 0)
        {
            reverb.frames_to_ignore--;
        }

        reverb.current_frame ^= 1;
    }
}

} // namespace ultralib::n64::ead_reverb
