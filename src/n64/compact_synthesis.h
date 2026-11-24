// SPDX-License-Identifier: MIT

// The parts of the audio lists for the compact microcode that libultra's synthesizer (al_synth_compact.cpp) and the
// SBN engine's driver (sbn_player_synthesis.cpp) build the same way. The SBN engine's driver is a form of libultra's
// driver for that microcode.
//
// The templates take a voice of either model. Both models name the fields that the templates use alike.

#pragma once

#include <cstddef>
#include <cstdint>

#include "al_synth_constants.h"
#include "equal_power.h"
#include "ultralib/n64/al_delay.h"
#include "ultralib/n64/audio_list.h"
#include "ultralib/n64/audio_memory.h"

namespace ultralib::n64::compact_synthesis
{

// The buffers of a chunk in DMEM.
inline constexpr uint32_t kMainLeft = 0x4E0;
inline constexpr uint32_t kMainRight = 0x650;
inline constexpr uint32_t kAuxLeft = 0x7C0;
inline constexpr uint32_t kAuxRight = 0x930;

// The commands' first words.
inline constexpr uint32_t kOpLoad = 0x04000000;
inline constexpr uint32_t kOpSave = 0x06000000;
inline constexpr uint32_t kOpMix = 0x0C000000;
inline constexpr uint32_t kOpMove = 0x0A000000;
inline constexpr uint32_t kAddressMask = 0xFFFFFF;
inline constexpr uint32_t kDmemMask = 0xFFF;

// The rate of a ramp from `volume` to `target` over `count` samples: a straight line in steps of 8 samples, in a whole
// part and a 16-bit fraction. A falling ramp with no time holds its volume when `holds_without_time` is set, and
// otherwise drops at the fastest rate. Most games have the arithmetic in double precision. Mario Tennis has it in
// single precision, at 0x802F26B8. The code is at 0x80047910 in Pokémon Stadium.
int16_t Rate(double volume, double target, int32_t count, uint16_t& rate_low, bool holds_without_time);
int16_t Rate(float volume, float target, int32_t count, uint16_t& rate_low, bool holds_without_time);

// Where a ramp has reached after `samples` samples. The code is at 0x80047A98 in Pokémon Stadium.
inline int16_t Ramp(int16_t volume, int32_t samples, int16_t rate_high, uint16_t rate_low)
{
    const int32_t steps = samples >> 3;
    if (steps == 0)
    {
        return volume;
    }

    const auto low = static_cast<int32_t>(static_cast<uint32_t>(rate_low) * static_cast<uint32_t>(steps));

    return static_cast<int16_t>(volume + (low >> 16) + rate_high * steps);
}

// Sets up the gain and coefficients of a delay's low-pass filter for its cutoff, and writes the coefficients to their
// place in `memory`. The standard form of libultra's synthesizer sets up its filters the same way.
void InitLowPass(AudioMemory& memory, AlLowPass& low_pass);

// Appends a command that loads the `bytes` bytes at `memory_in` to DMEM at `dmem`, from the 8-byte boundary at or
// before `memory_in`. Returns the distance from that boundary to `memory_in`.
inline uint32_t PushAlignedLoad(AudioList& list, int64_t memory_in, int32_t bytes, uint32_t dmem)
{
    const auto location = static_cast<uint32_t>(memory_in);
    const uint32_t align = location & 7;
    const uint32_t total = static_cast<uint32_t>(bytes) + align;
    list.commands.push_back({kOpLoad | (((total - (total & 7) + 8) & kDmemMask) << 12) | dmem, location - align});

    return align;
}

// Appends the commands that load `count` samples of a delay line of `length` samples at `base`, from `position`, to
// DMEM at `buffer`. A load past the end of the line goes on at its start. A negative position counts back from the end.
inline void PushDelayLoad(AudioList& list, uint32_t base, uint32_t length, int64_t position, int buffer, int count)
{
    const al_synth_constants::FxRun run = al_synth_constants::SplitFxRun(base, length, position, count);
    if (run.after > 0)
    {
        const auto before = static_cast<uint32_t>(run.before) << 1;
        const auto after = static_cast<uint32_t>(run.after) << 1;
        list.commands.push_back(
            {kOpLoad | ((before & kDmemMask) << 12) | (static_cast<uint32_t>(buffer) & kDmemMask), run.address});
        list.commands.push_back(
            {kOpLoad | ((after & kDmemMask) << 12) | ((static_cast<uint32_t>(buffer) + before) & kDmemMask), base});
    }
    else
    {
        list.commands.push_back({kOpLoad | ((static_cast<uint32_t>(count << 1) & kDmemMask) << 12) |
                                     (static_cast<uint32_t>(buffer) & kDmemMask),
                                 run.address});
    }
}

// Appends a resampler's command at `ratio`, from the samples at `in` in DMEM. `out_bits` selects the output buffer. The
// command after the first one continues from the resampler's state, and `first` is cleared.
void PushResample(AudioList& list, uint32_t state, bool& first, float ratio, uint32_t in, uint32_t out_bits);

// The level of one side of a voice at `volume`, from the equal-power table at `pan`.
inline int16_t SideLevel(int16_t volume, std::size_t pan)
{
    return static_cast<int16_t>((volume * kEqualPower[pan]) >> 15);
}

// The ratio of a chorus's resampler for the next chunk, and the samples that the resampler reads.
struct ChorusStep
{
    float ratio = 1.0F;
    int32_t samples = 0;
};

// Moves a delay's chorus modulation on by a chunk of `chunk` samples: a triangle from -1 to 1 at the chorus's rate. The
// definition is in compact_synthesis.cpp. That file and al_synth_compact.cpp build to follow the FPU's rounding mode at
// run time. An inline copy would build with the settings of each model's file, and the linker would keep one of the two
// copies.
ChorusStep StepChorus(AlDelay& delay, int chunk);

// Appends the ADPCM decoder's command for `tsam` samples of a voice into DMEM at `out`, after the load of `bytes` bytes
// of its frames.
template <typename Voice>
void PushAdpcm(AudioList& list, Voice& voice, int tsam, int bytes, int16_t out, uint32_t flags)
{
    uint32_t align = 0;
    if (bytes > 0)
    {
        align = PushAlignedLoad(list, voice.memory_in, bytes, 0);
    }

    if ((flags & kAudioFlagLoop) != 0)
    {
        list.SetLoop(voice.decoder_loop_state);
    }

    list.commands.push_back({static_cast<uint32_t>(kAudioAdpcm) << 24 | (voice.decoder_state & kAddressMask),
                             (flags << 28) | ((static_cast<uint32_t>(tsam * 2) & kDmemMask) << 16) |
                                 ((align & 0xF) << 12) | (static_cast<uint32_t>(out) & kDmemMask)});
    voice.decoder_first = false;
}

// Appends the envelope mixer's commands of a voice. `refresh` first sets its targets, its rates and its levels.
template <typename Voice>
void PushEnvMixer(AudioList& list, const Voice& voice, bool refresh)
{
    if (!refresh)
    {
        list.EnvMixer(0, voice.mixer_state);

        return;
    }

    constexpr uint32_t kSetTargetLeft = 0x09000000;
    constexpr uint32_t kSetCurrentLeft = 0x09060000;
    constexpr uint32_t kSetTargetRight = 0x09040000;
    constexpr uint32_t kEnvMixerInit = 0x03010000;
    const auto pair = [](int16_t high, uint16_t low)
    {
        return (static_cast<uint32_t>(static_cast<uint16_t>(high)) << 16) | low;
    };

    list.commands.push_back(
        {kSetTargetLeft | static_cast<uint16_t>(voice.target_left), pair(voice.rate_high_left, voice.rate_low_left)});
    list.commands.push_back({kSetCurrentLeft | static_cast<uint16_t>(voice.current_left),
                             pair(voice.dry, static_cast<uint16_t>(voice.wet))});
    list.commands.push_back({kSetTargetRight | static_cast<uint16_t>(voice.target_right),
                             pair(voice.rate_high_right, voice.rate_low_right)});
    list.commands.push_back({kEnvMixerInit | static_cast<uint16_t>(voice.current_right), voice.mixer_state});
}

// Appends the commands of a delay's low-pass filter on the buffer at `buffer`.
inline void PushLowPass(AudioList& list, AlLowPass& low_pass, int buffer)
{
    list.LoadAdpcm(32, low_pass.coefficients_address);
    list.commands.push_back({static_cast<uint32_t>(kAudioPoleFilter) << 24 | (low_pass.first ? 1U << 16 : 0U) |
                                 static_cast<uint16_t>(low_pass.gain),
                             ((static_cast<uint32_t>(buffer) >> 8) << 24) | (low_pass.state & kAddressMask)});
    low_pass.first = false;
}

} // namespace ultralib::n64::compact_synthesis
