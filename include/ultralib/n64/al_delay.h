// SPDX-License-Identifier: MIT

// A delay of an effect on the auxiliary bus. The synthesizer of libultra and the driver of the SBN engine keep delays
// in this form. The SDK names these ALDelay and ALLowPass.

#pragma once

#include <array>
#include <cstdint>

namespace ultralib::n64
{

// A delay's low-pass filter: its cutoff, the gain and coefficients that the cutoff gives, their place in main memory,
// and the filter's state there. `first` marks the filter's first command. That command starts from a clear state.
struct AlLowPass
{
    int16_t cutoff = 0;
    int16_t gain = 0;
    std::array<int16_t, 16> coefficients = {};
    uint32_t coefficients_address = 0;
    uint32_t state = 0;
    bool first = true;
};

// A delay: its input and output in the effect's delay line, its feed-forward, feedback and gain, the chorus that
// modulates its length, its low-pass filter and the resampler of the chorus.
struct AlDelay
{
    uint32_t input = 0;
    uint32_t output = 0;
    int16_t feed_forward = 0;
    int16_t feedback = 0;
    int16_t gain = 0;
    float mod_increment = 0.0F;
    float mod_value = 0.0F;
    int32_t mod_delta = 0;
    float mod_gain = 0.0F;
    bool has_low_pass = false;
    AlLowPass low_pass;
    bool has_resampler = false;
    uint32_t resampler_state = 0;
    float resampler_delta = 0.0F;
    bool resampler_first = true;
};

} // namespace ultralib::n64
