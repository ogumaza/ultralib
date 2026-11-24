// SPDX-License-Identifier: MIT

// The reverbs in the audio lists of the model of the later EAD engine: the commands that load, downsample, filter,
// send and save each reverb's samples in an update. ead_later_synthesis.cpp contains the rest of the lists.

#include <cstddef>
#include <cstdint>

#include "ead_constants.h"
#include "ead_later_constants.h"
#include "ead_lists.h"
#include "ead_reverb.h"
#include "ultralib/n64/ead_later.h"

namespace ultralib::n64
{

namespace
{

using ead_constants::kFrameSamples;
using ead_later_constants::kOneChannel;
using ead_later_constants::kTwoChannels;
using ead_lists::Filter;
using ead_lists::Halve;
using ead_lists::Load;
using ead_lists::Mix;
using ead_lists::Move;
using ead_lists::Resample;
using ead_lists::Save;
using ead_lists::SetBuffer;

constexpr uint32_t kSmallTemp = 0x3C0;

// A transfer for the ring buffers that loads from `address` to `dmem`.
auto LoadTo(AudioList& list)
{
    return [&list](uint32_t dmem, uint32_t address, uint32_t size)
    {
        Load(list, address, dmem, size);
    };
}

} // namespace

void EadLaterPlayer::StartReverbItems(AudioMemory& memory, int length, int update, int reverb_index)
{
    Reverb& reverb = reverbs_[static_cast<std::size_t>(reverb_index)];
    ReverbItem& item = reverb.items[reverb.current_frame][static_cast<std::size_t>(update)];
    if (!majora_ && reverb.downsample >= 2 && reverb.frames_to_ignore == 0)
    {
        ead_reverb::ThinSamples(memory, reverb, item);
    }

    const int start = reverb.next_position;
    ead_reverb::PlaceItem(reverb, item, length);
    if (reverb.second_delay != 0)
    {
        int second = reverb.second_delay + start;
        if (second >= reverb.buffer_samples)
        {
            second -= reverb.buffer_samples;
        }

        ReverbItem& later = reverb.second_items[reverb.current_frame][static_cast<std::size_t>(update)];
        ead_reverb::SplitItem(later, second, length / reverb.downsample, reverb.buffer_samples);
    }
}

// Loads both parts of a reverb item from the ring buffers of a reverb. The left side goes to `dmem` and the right side
// one channel after it.
void EadLaterPlayer::LoadReverbItem(AudioList& list, const Reverb& reverb, const ReverbItem& item, uint32_t dmem)
{
    ead_reverb::TransferParts(reverb, item, dmem, kOneChannel, LoadTo(list));
}

// Saves both parts of a reverb item to the ring buffers of a reverb. The left side comes from `dmem` and the right side
// from one channel after it.
void EadLaterPlayer::SaveReverbItem(AudioList& list, const Reverb& reverb, const ReverbItem& item, uint32_t dmem)
{
    const auto save = [&list](uint32_t from, uint32_t address, uint32_t size)
    {
        Save(list, from, address, size);
    };

    ead_reverb::TransferParts(reverb, item, dmem, kOneChannel, save);
}

void EadLaterPlayer::LoadReverbSamples(AudioList& list, const Reverb& reverb, int length, int update)
{
    const ReverbItem& item = reverb.items[reverb.current_frame][static_cast<std::size_t>(update)];
    const auto resample = [&](uint32_t in, uint32_t out, uint32_t size, int side)
    {
        SetBuffer(list, in, out, size);
        Resample(list, reverb.resample_flags, reverb.downsample_pitch,
                 reverb.resample_states[static_cast<std::size_t>(side)]);
    };

    ead_reverb::LoadSamples(reverb, item, length, {dmem_.wet_left, dmem_.wet_temp, kOneChannel}, LoadTo(list),
                            resample);
}

void EadLaterPlayer::SaveReverbSamples(AudioList& list, Reverb& reverb, int update)
{
    const ReverbItem& item = reverb.items[reverb.current_frame][static_cast<std::size_t>(update)];
    if (reverb.downsample == 1)
    {
        SaveReverbItem(list, reverb, item, dmem_.wet_left);
    }
    else if (majora_)
    {
        int rate = reverb.downsample;
        int count = 13 * kFrameSamples;
        while (rate >= 2)
        {
            Halve(list, static_cast<uint32_t>(count), dmem_.wet_left, dmem_.wet_left);
            Halve(list, static_cast<uint32_t>(count), dmem_.wet_right, dmem_.wet_right);
            rate >>= 1;
            count >>= 1;
        }

        const auto first = static_cast<uint32_t>(item.length_a);
        const auto second = static_cast<uint32_t>(item.length_b);
        const auto place = static_cast<uint32_t>(item.start) * 2;
        if (first != 0)
        {
            SaveAligned(list, dmem_.wet_left, first, reverb.left + place);
            SaveAligned(list, dmem_.wet_right, first, reverb.right + place);
        }

        if (second != 0)
        {
            SaveAligned(list, dmem_.wet_left + first, second, reverb.left);
            SaveAligned(list, dmem_.wet_right + first, second, reverb.right);
        }
    }
    else
    {
        Save(list, dmem_.wet_left, item.downsample_left, kTwoChannels);
    }

    reverb.resample_flags = 0;
}

void EadLaterPlayer::SaveAligned(AudioList& list, uint32_t dmem, uint32_t size, uint32_t address)
{
    const uint32_t end_extra = (address + size) & 0xF;
    if (end_extra != 0)
    {
        Load(list, address + size - end_extra, dmem_.temp, 0x10);
        Move(list, dmem, kSmallTemp, size);
        Move(list, dmem_.temp + end_extra, size + kSmallTemp, 0x10 - end_extra);
        size += 0x10 - end_extra;
        dmem = kSmallTemp;
    }

    const uint32_t start_extra = address & 0xF;
    if (start_extra != 0)
    {
        Load(list, address - start_extra, dmem_.temp, 0x10);
        Move(list, dmem, start_extra + dmem_.temp, size);
        size += start_extra;
        dmem = dmem_.temp;
    }

    Save(list, dmem, address - start_extra, size);
}

void EadLaterPlayer::SendReverb(AudioList& list, const Reverb& reverb, int update)
{
    if (static_cast<std::size_t>(reverb.send_to) >= reverbs_.size())
    {
        return;
    }

    const Reverb& target = reverbs_[static_cast<std::size_t>(reverb.send_to)];
    if (target.downsample != 1)
    {
        return;
    }

    const ReverbItem& item = target.items[target.current_frame][static_cast<std::size_t>(update)];
    LoadReverbItem(list, target, item, dmem_.wet_temp);
    Mix(list, kTwoChannels, static_cast<uint16_t>(reverb.send_gain), dmem_.wet_left, dmem_.wet_temp);
    SaveReverbItem(list, target, item, dmem_.wet_temp);
}

// Saves the wet samples of a reverb and sends them to the reverb that it feeds. The form of Majora's Mask sends them
// before the save. Ocarina of Time sends them after it.
void EadLaterPlayer::SaveAndSendReverb(AudioList& list, Reverb& reverb, int update)
{
    if (majora_ && reverb.send_to != -1)
    {
        SendReverb(list, reverb, update);
    }

    SaveReverbSamples(list, reverb, update);
    if (!majora_ && reverb.send_to != -1)
    {
        SendReverb(list, reverb, update);
    }
}

// Adds the commands that start a reverb in an update: the load of its delayed samples, their mix into the dry output,
// the decay, the leaks between the sides and the second delay.
void EadLaterPlayer::BuildReverbStart(AudioList& list, Reverb& reverb, int length, int update)
{
    const bool second = reverb.second_delay != 0;
    LoadReverbSamples(list, reverb, length, update);
    Mix(list, kTwoChannels, static_cast<uint16_t>(reverb.volume), dmem_.wet_left, dmem_.left);
    if (second)
    {
        Move(list, dmem_.wet_left, dmem_.wet_temp, kTwoChannels);
    }

    Mix(list, kTwoChannels, (reverb.decay + 0x8000U) & 0xFFFF, dmem_.wet_left, dmem_.wet_left);
    if ((reverb.leak_right_to_left != 0 || reverb.leak_left_to_right != 0) && config_.output != EadLaterOutput::kMono)
    {
        Move(list, dmem_.wet_left, dmem_.wet_scratch, kOneChannel);
        Mix(list, kOneChannel, static_cast<uint16_t>(reverb.leak_right_to_left), dmem_.wet_right, dmem_.wet_left);
        Mix(list, kOneChannel, static_cast<uint16_t>(reverb.leak_left_to_right), dmem_.wet_scratch, dmem_.wet_right);
    }

    if (!second)
    {
        return;
    }

    SaveAndSendReverb(list, reverb, update);
    if (reverb.downsample == 1)
    {
        LoadReverbItem(list, reverb, reverb.second_items[reverb.current_frame][static_cast<std::size_t>(update)],
                       dmem_.wet_left);
    }

    Mix(list, kTwoChannels, static_cast<uint16_t>(reverb.second_gain), dmem_.wet_temp, dmem_.wet_left);
}

// Adds the commands that end a reverb in an update, after its voices: its filters and the save of its samples.
void EadLaterPlayer::BuildReverbEnd(AudioList& list, Reverb& reverb, int length, int update)
{
    const uint32_t size = static_cast<uint32_t>(length) * 2;
    if (reverb.filter_left != 0)
    {
        Filter(list, 2, size, reverb.filter_left);
        Filter(list, reverb.resample_flags, dmem_.wet_left, reverb.filter_left_state);
    }

    if (reverb.filter_right != 0)
    {
        Filter(list, 2, size, reverb.filter_right);
        Filter(list, reverb.resample_flags, dmem_.wet_right, reverb.filter_right_state);
    }

    if (reverb.second_delay != 0)
    {
        SaveReverbItem(list, reverb, reverb.second_items[reverb.current_frame][static_cast<std::size_t>(update)],
                       dmem_.wet_left);
    }
    else
    {
        SaveAndSendReverb(list, reverb, update);
    }
}

} // namespace ultralib::n64
