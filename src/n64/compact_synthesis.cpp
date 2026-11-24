// SPDX-License-Identifier: MIT

// The floating-point parts of the audio lists for the compact microcode. This file builds with the rounding mode at run
// time, for the games whose sound threads round toward zero.

#include "compact_synthesis.h"

#include <array>
#include <cstddef>
#include <cstdint>

#include "xsflib/binary.h"

#include "al_synth_constants.h"
#include "ultralib/n64/audio_list.h"
#include "ultralib/n64/audio_memory.h"

namespace ultralib::n64::compact_synthesis
{

namespace
{

template <typename Real>
int16_t RateIn(Real volume, Real target, int32_t count, uint16_t& rate_low, bool holds_without_time)
{
    if (count == 0)
    {
        if (volume <= target)
        {
            rate_low = 0xFFFF;

            return 0x7FFF;
        }

        rate_low = 0;

        return holds_without_time ? int16_t{0} : int16_t{-0x8000};
    }

    constexpr auto kStep = static_cast<Real>(8.0);
    constexpr auto kLowScale = static_cast<Real>(65535.0);
    const Real inverse = static_cast<Real>(1.0) / static_cast<Real>(count);
    if (target < static_cast<Real>(1.0))
    {
        target = static_cast<Real>(1.0);
    }

    if (volume <= static_cast<Real>(0.0))
    {
        volume = static_cast<Real>(1.0);
    }

    const Real step = (target - volume) * inverse * kStep;
    const auto whole = static_cast<int16_t>(static_cast<int32_t>(step));
    const Real above = step - static_cast<Real>(whole) + static_cast<Real>(1.0);
    const auto carry = static_cast<int16_t>(static_cast<int32_t>(above));
    rate_low = static_cast<uint16_t>(static_cast<uint32_t>(kLowScale * (above - static_cast<Real>(carry))));

    return static_cast<int16_t>(whole - 1 + carry);
}

} // namespace

int16_t Rate(double volume, double target, int32_t count, uint16_t& rate_low, bool holds_without_time)
{
    return RateIn(volume, target, count, rate_low, holds_without_time);
}

int16_t Rate(float volume, float target, int32_t count, uint16_t& rate_low, bool holds_without_time)
{
    return RateIn(volume, target, count, rate_low, holds_without_time);
}

void InitLowPass(AudioMemory& memory, AlLowPass& low_pass)
{
    constexpr int kScale = 16384;
    std::array<int16_t, 16>& coefficients = low_pass.coefficients;
    const int32_t scaled = low_pass.cutoff * kScale;
    const auto fc = static_cast<int16_t>(scaled >> 15);
    low_pass.gain = static_cast<int16_t>(kScale - fc);
    low_pass.first = true;
    coefficients.fill(0);
    coefficients[8] = fc;
    double running = static_cast<double>(fc) / kScale;
    const double step = running;
    for (std::size_t i = 9; i < coefficients.size(); i++)
    {
        running *= step;
        coefficients[i] = static_cast<int16_t>(static_cast<int32_t>(running * kScale));
    }

    std::array<uint8_t, 32> bytes = {};
    for (std::size_t i = 0; i < coefficients.size(); i++)
    {
        xsflib::StoreBe16(bytes.data() + i * 2, static_cast<uint16_t>(coefficients[i]));
    }

    memory.Write(low_pass.coefficients_address, bytes.data(), bytes.size());
}

void PushResample(AudioList& list, uint32_t state, bool& first, float ratio, uint32_t in, uint32_t out_bits)
{
    const auto increment = static_cast<int32_t>(ratio * al_synth_constants::kUnityPitch);
    list.commands.push_back({static_cast<uint32_t>(kAudioResample) << 24 | (state & kAddressMask),
                             (first ? 1U << 30 : 0U) | ((static_cast<uint32_t>(increment) & 0xFFFF) << 14) |
                                 ((in & kDmemMask) << 2) | out_bits});
    first = false;
}

ChorusStep StepChorus(AlDelay& delay, int chunk)
{
    constexpr double kRange = 2.0;
    const int32_t length = static_cast<int32_t>(delay.output - delay.input);
    delay.mod_value += delay.mod_increment * static_cast<float>(chunk);
    delay.mod_value = delay.mod_value > kRange ? static_cast<float>(delay.mod_value - kRange * 2) : delay.mod_value;
    float wave = delay.mod_value < 0 ? -delay.mod_value : delay.mod_value;
    wave = static_cast<float>(wave - kRange / 2);
    float delta = delay.mod_gain * wave;

    delta /= static_cast<float>(length);
    delta = static_cast<float>(static_cast<int32_t>(delta * al_synth_constants::kUnityPitch));
    delta = delta / al_synth_constants::kUnityPitch;

    ChorusStep step;
    step.ratio = static_cast<float>(1.0 - delta);
    const float exact = delay.resampler_delta + step.ratio * static_cast<float>(chunk);
    step.samples = static_cast<int32_t>(exact);
    delay.resampler_delta = exact - static_cast<float>(step.samples);

    return step;
}

} // namespace ultralib::n64::compact_synthesis
