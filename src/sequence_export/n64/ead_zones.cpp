// SPDX-License-Identifier: MIT

#include <algorithm>
#include <array>
#include <cmath>
#include <span>
#include <vector>

#include "ead_shared.h"
#include "n64/ead_common.h"
#include "n64/ead_constants.h"
#include "rsp_audio.h"
#include "ultralib/sequence_export/n64/ead.h"
#include "ultralib/sequence_export/n64/envelope.h"

namespace ultralib::sequence_export::n64
{

namespace
{

namespace n64 = ultralib::n64;
using ead::Be16At;
using ead::IsLater;
using ead::kEnvelopeTop;
using ead::kKeyOffset;
using ead::kPitchScaleSteps;
using ead::kRootKey;
using ead::kVibratoDepthUnit;
using ead::kVibratoPeakSteps;
using ead::kVibratoPeriodUnits;
using ead::ReleaseFall;
using n64::EadEngine;

// A channel's default release rate.
constexpr int kDefaultReleaseRate = 0x20;

// The highest resampling rate, against the output's rate.
constexpr double kHighestRate = 3.99992;

// What each row of the later engine's low-pass table does at 32006 Hz: where the table's 8 taps fall 3 dB below their
// gain at 0 Hz, and that gain. A two-pole filter stands in for the taps. The first row and the last pass everything
// below half the rate.
struct LaterLowPass
{
    double hz = 0.0;
    double gain = 1.0;
};

constexpr std::array<LaterLowPass, 16> kLaterLowPasses = {{{0.0, 1.0},
                                                           {1850.0, 1.0},
                                                           {1990.0, 1.0},
                                                           {2290.0, 1.0},
                                                           {2980.0, 0.951},
                                                           {4360.0, 0.854},
                                                           {5770.0, 0.707},
                                                           {6860.0, 0.670},
                                                           {7850.0, 0.640},
                                                           {8730.0, 0.610},
                                                           {9580.0, 0.639},
                                                           {10600.0, 0.624},
                                                           {11710.0, 0.602},
                                                           {12530.0, 0.603},
                                                           {13270.0, 0.670},
                                                           {0.0, 0.846}}};

// A note's amplitude when the velocity, the volumes and the envelope stand at their tops. Wave Race 64's code at
// 0x800BA758 multiplies the square of the velocity, without scaling it, by the pan. It writes the top 11 of 16 bits,
// and the mixer's unit is 32768. Mario Kart 64's code at 0x800BC7B0 multiplies a velocity's share of 1 by 4096, and the
// mixer's unit is 4096 times 16.
double FullScale(EadLevels levels)
{
    return levels == EadLevels::kEnvelopeSquared ? 127.0 * 127.0 / 32768.0 : 1.0;
}

// The straight lines of an envelope, from a level of 0, up to the end of the list. A list that goes back holds its last
// level there. The later engine scales each of an envelope's delays by its updates in a frame, against 4. The two
// builds in between keep the shortest delays as they stand. Pokémon Stadium 2's code at 0x8000E810 starts a note whose
// list opens with a skipped point at its first update's level, with no ramp from silence.
EnvelopeShape ShapeOf(std::span<const n64::EadEnvelope::Point> envelope, EadEngine engine, EadLevels levels,
                      int updates_per_frame, bool envelope_skip)
{
    EnvelopeShape shape;
    shape.squared_lines = levels != EadLevels::kVolumesSquared;
    shape.points.push_back({0.0, 0.0});
    double time = 0.0;
    bool start_at_target = false;
    for (const n64::EadEnvelope::Point& point : envelope)
    {
        if (envelope_skip && point.delay == n64::kEnvelopeSkip)
        {
            start_at_target = start_at_target || shape.points.size() == 1;
            continue;
        }

        if (point.delay <= 0)
        {
            shape.ends = point.delay == n64::kEnvelopeDisable;
            break;
        }

        const int delay = IsLater(engine) || (engine != EadEngine::kEarly && point.delay >= 4)
                              ? point.delay * updates_per_frame / 4
                              : point.delay;
        const double top = std::clamp(static_cast<double>(point.target) / kEnvelopeTop, 0.0, 1.0);
        const auto level_of = [&](double value)
        {
            return levels == EadLevels::kEarly ? value : value * value;
        };

        if (start_at_target && shape.points.size() == 1)
        {
            const double first = level_of(top / std::max(delay, 1));
            shape.points[0].level = first;
            if (delay > 1)
            {
                shape.points.push_back({1.0, first});
            }
        }

        time += std::max(delay, 1);
        shape.points.push_back({time, level_of(top)});
    }

    return shape;
}

// A vibrato's LFO. The engine's curve moves further up than down. The LFO takes the middle of the curve as the note's
// pitch, and half its span as its depth. Returns the shift in cents of that middle.
double FitVibrato(Lfo& lfo, const EadPresetKey& key, double updates_per_second, EadEngine engine, bool inverse_vibrato)
{
    lfo.hz = updates_per_second * key.vibrato_rate / kVibratoPeriodUnits;
    lfo.delay_ms = key.vibrato_delay * 1000.0 / updates_per_second;
    const double extent = key.vibrato_depth / kVibratoDepthUnit;
    if (inverse_vibrato)
    {
        // Pokémon Stadium 2's code at 0x8001BC5C: the scale is 65536 / (wave * depth + 65536), for a wave of up to
        // 32767 either way. The curve moves further up than down.
        const double up = -1200.0 * std::log2(std::max(1.0 - extent / 2.0, 1.0 / 64.0));
        const double down = -1200.0 * std::log2(1.0 + extent / 2.0);
        lfo.depth = (up - down) / 2.0;

        return (up + down) / 2.0;
    }

    if (IsLater(engine))
    {
        // The later engine's curve is a sine in the scale between one plus the depth and its inverse. Its middle stays
        // within a cent of the note for any depth that a sequence uses.
        lfo.depth = 1200.0 * std::log2(1.0 + extent);
        return 0.0;
    }

    const auto cents_at = [&](double steps)
    {
        return 1200.0 * std::log2(std::max(1.0 + extent * (std::pow(2.0, steps / kPitchScaleSteps) - 1.0), 1.0 / 64.0));
    };

    const double up = cents_at(kVibratoPeakSteps);
    const double down = cents_at(-kVibratoPeakSteps);
    lfo.depth = (up - down) / 2.0;

    return (up + down) / 2.0;
}

} // namespace

std::vector<Zone> EadZones(const EadPresetKey& key, const n64::EadBank& bank, const EadPerformSettings& settings,
                           std::vector<int32_t>& sample_of_zone)
{
    std::vector<Zone> zones;
    sample_of_zone.clear();
    const int updates_per_frame = settings.clock.updates_per_frame;
    const double updates = updates_per_frame * settings.clock.frame_rate;
    const double rate_ratio = settings.clock.wave_rate / settings.clock.output_rate;
    const auto fade_of = [&](int rate)
    {
        return ReleaseFall(settings.engine, rate, updates_per_frame);
    };

    Lfo lfo;
    double vibrato_shift = 0.0;
    if (key.vibrato_depth != 0)
    {
        vibrato_shift = FitVibrato(lfo, key, updates, settings.engine, settings.inverse_vibrato);
    }

    const auto add = [&](int low_key, int high_key, const n64::EadTunedSample& tuned,
                         std::span<const n64::EadEnvelope::Point> envelope, int release_rate, double pan, bool drum)
    {
        if (tuned.sample == n64::kNoObject || tuned.tuning <= 0.0F || low_key > high_key)
        {
            return;
        }

        Zone zone;
        zone.low_key = low_key;
        zone.high_key = high_key;
        zone.root_key = drum ? low_key : kRootKey;
        zone.tune_cents = 1200.0 * std::log2(static_cast<double>(tuned.tuning)) + vibrato_shift;
        zone.follows_key = !drum;
        zone.pan = pan;
        if (key.vibrato_depth != 0)
        {
            zone.vibrato = lfo;
        }

        const EnvelopeShape shape =
            ShapeOf(envelope, settings.engine, settings.levels, updates_per_frame, settings.envelope_skip);
        const LaterLowPass& low_pass = kLaterLowPasses[static_cast<std::size_t>(key.low_pass & 0xF)];
        zone.low_pass_hz = low_pass.hz;
        zone.gain =
            FitEnvelope(zone, shape, updates, fade_of(release_rate)) * FullScale(settings.levels) * low_pass.gain;
        if (drum)
        {
            zones.push_back(zone);
            sample_of_zone.push_back(tuned.sample);
            return;
        }

        const double limit =
            kRootKey + 12.0 * std::log2(kHighestRate / (static_cast<double>(tuned.tuning) * rate_ratio));
        AddUpToResamplerLimit(zones, zone, static_cast<int>(std::ceil(limit)),
                              1200.0 * std::log2(kHighestRate / rate_ratio) + vibrato_shift);
        sample_of_zone.resize(zones.size(), tuned.sample);
    };

    if (key.program < 0)
    {
        for (std::size_t note = 0; note < bank.drum_notes.size() && note + kKeyOffset <= 127; note++)
        {
            const n64::EadDrum* found = n64::ead_common::NoteDrum(bank, static_cast<int>(note));
            if (found == nullptr || found->envelope == n64::kNoObject)
            {
                continue;
            }

            const n64::EadDrum& drum = *found;

            const double weight = key.drum_pan_weight / 128.0;
            const double pan = std::clamp((drum.pan / 64.0 - 1.0) * (1.0 - weight), -1.0, 1.0);
            const int midi_key = static_cast<int>(note) + kKeyOffset;
            add(midi_key, midi_key, drum.sound, bank.envelopes[static_cast<std::size_t>(drum.envelope)].points,
                drum.release_rate, pan, true);
        }

        return zones;
    }

    const n64::EadInstrument& instrument = *n64::ead_common::ProgramInstrument(bank, key.program);
    std::vector<n64::EadEnvelope::Point> channel_envelope;
    if (key.envelope >= 0)
    {
        // A channel's envelope is in the sequence.
        for (std::size_t place = static_cast<std::size_t>(key.envelope); place + 4 <= settings.sequence.size();
             place += 4)
        {
            const n64::EadEnvelope::Point point = {static_cast<int16_t>(Be16At(settings.sequence, place)),
                                                   static_cast<int16_t>(Be16At(settings.sequence, place + 2))};
            channel_envelope.push_back(point);
            if (point.delay <= 0)
            {
                break;
            }
        }
    }

    std::span<const n64::EadEnvelope::Point> envelope = channel_envelope;
    if (key.envelope < 0)
    {
        envelope = instrument.envelope != n64::kNoObject
                       ? std::span<const n64::EadEnvelope::Point>(
                             bank.envelopes[static_cast<std::size_t>(instrument.envelope)].points)
                       : std::span<const n64::EadEnvelope::Point>(n64::ead_constants::kDefaultEnvelopePoints);
    }

    int release = key.release_rate >= 0 ? key.release_rate : instrument.release_rate;
    if (release == 0 && !IsLater(settings.engine))
    {
        release = kDefaultReleaseRate;
    }

    const int top_key = 127;
    const int low_end = std::min(instrument.normal_range_low + kKeyOffset, top_key + 1);
    const int high_start = std::min(instrument.normal_range_high + 1 + kKeyOffset, top_key + 1);
    add(0, low_end - 1, instrument.low, envelope, release, 0.0, false);
    add(low_end, high_start - 1, instrument.normal, envelope, release, 0.0, false);
    add(high_start, top_key, instrument.high, envelope, release, 0.0, false);

    return zones;
}

} // namespace ultralib::sequence_export::n64
