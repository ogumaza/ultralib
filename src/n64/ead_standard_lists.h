// SPDX-License-Identifier: MIT

// The commands for the standard microcode that the early EAD engine and the middle engine share. The middle engine
// uses that microcode in Wave Race 64. The commands load and decode a pass of ADPCM frames, halve a two-part update and
// delay one side of a voice in headset mode.

#pragma once

#include <cstdint>

#include "ntdlib/rounding.h"

#include "ead_adpcm_pass.h"
#include "ead_common.h"
#include "ead_constants.h"
#include "ultralib/n64/audio_list.h"

namespace ultralib::n64::ead_standard_lists
{

// Loads the compressed frames of a pass into DMEM. The load starts at their address rounded down to 16 bytes. Returns
// the misalignment of the address.
inline uint32_t LoadFrames(AudioList& list, uint32_t sample_address, int32_t position, const EadAdpcmPass& pass)
{
    using ead_constants::kFrameSamples;
    using ead_standard_dmem::kCompressed;

    if (pass.frames == 0)
    {
        return 0;
    }

    const int frame = (position - pass.ignored_first + kFrameSamples) / kFrameSamples;
    const uint32_t address = sample_address + static_cast<uint32_t>(frame) * 9;
    const uint32_t misaligned = address & 0xF;
    list.SetBuffer(0, kCompressed, 0, static_cast<uint32_t>(pass.frames) * 9 + misaligned);
    list.LoadBuffer(address - misaligned);

    return misaligned;
}

// Appends the commands that decode the frames of a pass, loaded with `misaligned` bytes before them. A part's first
// pass decodes to the start of the decoded samples. A later pass decodes after the part's `written` bytes, rounded up
// to 32, and moves its samples to follow them.
inline void DecodePass(AudioList& list, const EadAdpcmPass& pass, uint32_t misaligned, bool first, uint32_t written,
                       uint8_t flags, uint32_t adpcm_state)
{
    using ead_standard_dmem::kCompressed;
    using ead_standard_dmem::kDecoded;

    const auto bytes = static_cast<uint32_t>(pass.to_decode) * 2;
    if (first)
    {
        list.SetBuffer(0, kCompressed + misaligned, kDecoded, bytes);
        list.Adpcm(flags, adpcm_state);

        return;
    }

    const uint32_t moved_to = ntdlib::AlignUp(written, 32U);
    list.SetBuffer(0, kCompressed + misaligned, kDecoded + moved_to, bytes);
    list.Adpcm(flags, adpcm_state);
    list.DmemMove(kDecoded + moved_to + static_cast<uint32_t>(pass.ignored_first) * 2, kDecoded + written,
                  static_cast<uint32_t>(pass.Samples()) * 2);
}

// Appends the commands that halve part `part` of a two-part update of `wanted` bytes: a resampling at a pitch of 0xFF60
// from `decoded`. The first part goes to kResampled, and `resampled_length` gets its bytes. The second part follows it.
// A first part that finishes the note clears the bytes after it. The final resampling reads from kResampled + 4.
inline void HalvePart(AudioList& list, int part, uint32_t decoded, uint32_t wanted, bool finished, uint32_t state,
                      uint32_t& resampled_length)
{
    using ead_standard_dmem::kResampled;
    using ead_standard_dmem::kResampled2;

    constexpr uint32_t kHalvingPitch = 0xFF60;
    if (part == 0)
    {
        list.SetBuffer(0, decoded, kResampled, wanted + 4);
        list.Resample(kAudioFlagInit, kHalvingPitch, state);
        resampled_length = wanted + 4;
        if (finished)
        {
            list.ClearBuffer(kResampled + resampled_length, wanted + 0x10);
        }

        return;
    }

    list.SetBuffer(0, decoded, kResampled2, wanted + 8);
    list.Resample(kAudioFlagInit, kHalvingPitch, state);
    list.DmemMove(kResampled2 + 4, kResampled + resampled_length, wanted + 4);
}

// Appends the commands of a headset delay to a voice's samples of `bytes` at the place for the delayed side.
// `pan_resample` and `pan_samples` are the voice's state of the resampler and the samples that it delays.
inline void BuildHeadsetDelay(AudioList& list, const ead_common::HeadsetDelay& delay, uint32_t bytes, bool init,
                              uint32_t pan_resample, uint32_t pan_samples)
{
    using ead_standard_dmem::kPanTemp;
    using ead_standard_dmem::kTemp;

    const uint32_t shift = delay.shift;
    const uint32_t previous_shift = delay.previous_shift;
    const uint32_t shifted = shift + bytes - previous_shift;
    if (!init)
    {
        // A small change of rate fits a change of the delay.
        const uint32_t pitch = ead_common::HeadsetResamplePitch(bytes, delay);
        if (previous_shift == 0)
        {
            list.DmemMove(kPanTemp, kTemp, 8);
            list.ClearBuffer(8, 8);
            list.DmemMove(kPanTemp, kTemp + 0x10, 0x10);
            list.SetBuffer(0, 0, kTemp, 32);
            list.SaveBuffer(pan_resample);
            list.SetBuffer(0, kPanTemp + 8, kTemp, shifted);
            list.Resample(0, pitch, pan_resample);
        }
        else
        {
            list.SetBuffer(0, kPanTemp, kTemp, shifted);
            list.Resample(0, pitch, pan_resample);
        }

        if (previous_shift != 0)
        {
            list.SetBuffer(0, kPanTemp, 0, previous_shift);
            list.LoadBuffer(pan_samples);
            list.DmemMove(kTemp, kPanTemp + previous_shift, shifted);
        }
        else
        {
            list.DmemMove(kTemp, kPanTemp, shifted);
        }
    }
    else
    {
        // The delay only moves the samples to the right.
        list.DmemMove(kPanTemp, kTemp, bytes);
        list.DmemMove(kTemp, kPanTemp + shift, bytes);
        list.ClearBuffer(kPanTemp, shift);
    }

    if (shift != 0)
    {
        list.SetBuffer(0, 0, kPanTemp + bytes, shift);
        list.SaveBuffer(pan_samples);
    }

    list.SetBuffer(0, 0, 0, bytes);
    list.Mixer(0, 0x7FFF, kPanTemp, delay.target);
}

} // namespace ultralib::n64::ead_standard_lists
