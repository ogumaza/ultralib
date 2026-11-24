// SPDX-License-Identifier: MIT

// What the conversion of EAD's sequences, its zones and its games share.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "xsflib/bytes.h"

#include "ultralib/n64/ead_script.h"

namespace ultralib::sequence_export::n64::ead
{

using ultralib::n64::EadEngine;

// A script's note 39 plays a sample at its tuning. MIDI's key 60 stands for it.
inline constexpr int kKeyOffset = 21;
inline constexpr int kRootKey = 60;

// The top of an envelope's scale.
inline constexpr double kEnvelopeTop = 32767.0;

// An engine's scale of pitch: 127 steps make an octave. A vibrato's curve is a triangle of 64 steps that peaks at 120
// of them, and the depth counts out of 4096. Its position advances by the rate in each update, and 65536 is a period.
inline constexpr double kPitchScaleSteps = 127.0;
inline constexpr double kVibratoPeakSteps = 120.0;
inline constexpr double kVibratoDepthUnit = 4096.0;
inline constexpr double kVibratoPeriodUnits = 65536.0;

// Whether `engine` is one of the later engine's builds.
inline bool IsLater(EadEngine engine)
{
    return engine == EadEngine::kLater || engine == EadEngine::kLaterMajora;
}

// How many frames a later engine's release takes from the top of the envelope, for each rate of its table. A rate of 0
// never falls.
inline double LaterReleaseFrames(int rate)
{
    constexpr std::array<double, 5> kShortest = {0.75, 0.66, 0.5, 0.33, 0.25};
    if (rate >= 251)
    {
        return kShortest[static_cast<std::size_t>(rate - 251)];
    }

    if (rate >= 128)
    {
        return 251.0 - rate;
    }

    if (rate >= 16)
    {
        return 4.0 * (143 - rate);
    }

    return rate >= 1 ? 60.0 * (23 - rate) : 0.0;
}

// How far a note's level falls in an update during its release, from a top of 1.
inline double ReleaseFall(EadEngine engine, int rate, int updates_per_frame)
{
    if (IsLater(engine))
    {
        const double frames = LaterReleaseFrames(rate);

        return frames > 0.0 ? 1.0 / (frames * updates_per_frame) : 0.0;
    }

    return engine == EadEngine::kEarly ? rate * 24.0 / kEnvelopeTop : rate * (3.0 / 1280.0) / updates_per_frame;
}

inline uint32_t Be16At(std::span<const uint8_t> bytes, std::size_t place)
{
    return place + 2 <= bytes.size() ? xsflib::LoadBe16(bytes.data() + place) : 0;
}

} // namespace ultralib::sequence_export::n64::ead
