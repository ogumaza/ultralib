// SPDX-License-Identifier: MIT

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <utility>
#include <vector>

#include "libmus_shared.h"
#include "ultralib/sequence_export/n64/envelope.h"
#include "ultralib/sequence_export/n64/libmus.h"

namespace ultralib::sequence_export::n64
{

namespace
{

using libmus::kKeyOfNote;
using libmus::kTopSteps;
using libmus::kVolumeShift;

// In the later player, a channel keeps one voice, and a new note cuts the last one (the code at 0x80054988 and
// 0x80054B04 in Gex 64). Every zone therefore cuts the last note of its channel. In the early player, a channel sends
// its sounding voice into its release and allocates a new voice for the next note (the code at 0x8003AE98 and
// 0x8003B3F4 in Pokémon Stadium). Its zones cut nothing.
constexpr int kChannelClass = 1;

constexpr double kFullVoice = 32768.0;

// A note of the early player starts at its peak if its attack takes less than this.
constexpr int kShortestEarlyAttack = 2;

// Cuts `shape` off at the level `top`: every higher level plays as `top`. The shape then stays at `top` from where its
// rise crosses that level to where its fall does. That stretch is taken out of the shape's updates, and its length goes
// in `held_updates`. A SoundFont's hold takes it.
void ClipShape(EnvelopeShape& shape, double top)
{
    std::vector<EnvelopePoint> clipped;
    for (std::size_t i = 0; i < shape.points.size(); i++)
    {
        const EnvelopePoint& point = shape.points[i];
        if (i > 0 && (shape.points[i - 1].level - top) * (point.level - top) < 0.0)
        {
            const EnvelopePoint& last = shape.points[i - 1];
            const double share = (top - last.level) / (point.level - last.level);
            clipped.push_back({last.updates + (point.updates - last.updates) * share, top});
        }

        clipped.push_back({point.updates, std::min(point.level, top)});
    }

    const auto at_top = [&](const EnvelopePoint& point)
    {
        return point.level >= top;
    };

    // A shape that ends at the top holds the note there, and nothing follows the stretch.
    const auto first = std::find_if(clipped.begin(), clipped.end(), at_top);
    const auto after = std::find_if_not(first, clipped.end(), at_top);
    double stretch = 0.0;
    if (first != clipped.end())
    {
        if (after != clipped.end())
        {
            stretch = std::prev(after)->updates - first->updates;
        }

        for (auto point = after; point != clipped.end(); ++point)
        {
            point->updates -= stretch;
        }

        clipped.erase(std::next(first), after);
    }

    shape.points = std::move(clipped);
    shape.held_updates = stretch;
}

} // namespace

Zone LibmusZone(const LibmusPresetKey& key, double detune, const LibmusSettings& settings, double ticks_per_second)
{
    Zone zone;
    zone.exclusive_class = settings.early_player ? 0 : kChannelClass;

    // A key's pitch is key - kKeyOfNote + detune semitones above the output's rate.
    const int root = static_cast<int>(std::lround(kKeyOfNote - detune));
    zone.root_key = std::clamp(root, 0, 127);
    zone.tune_cents = 100.0 * (zone.root_key - kKeyOfNote + detune);

    // The lines between the bytes, on a scale of 127. The later player counts an envelope's times in units of its
    // speed. Gex 64's speed command at 0x80053048 stores 0x400 / speed, and the code at 0x80054F48 scales the ticks by
    // it. A decay's count starts where the attack's stops, less the attack's units. The early player counts ticks.
    const auto [speed, first_byte, attack, peak, decay, sustain, release] = key.envelope;
    const double unit = settings.early_player ? 1.0 : std::max<int>(speed, 1);
    const int first = settings.early_player && attack < kShortestEarlyAttack ? peak : first_byte;
    const double top_ticks = std::max<int>(attack, 1) * unit;
    const double decay_end = std::max<int>(attack, 1) + std::max<int>(decay, 1) * unit;

    EnvelopeShape shape;
    shape.squared_lines = true;
    shape.points.push_back({0.0, first / 127.0});
    shape.points.push_back({top_ticks, peak / 127.0});
    if (decay_end > top_ticks)
    {
        shape.points.push_back({decay_end, sustain / 127.0});
    }
    else if (unit > 1.0)
    {
        // A slow attack ends after the decay's count has run out, and the envelope steps from the peak to the sustain
        // (the code at 0x80054FA8 in Gex 64). One tick stands for the step.
        shape.points.push_back({top_ticks + 1.0, sustain / 127.0});
    }

    if (key.top != 0)
    {
        ClipShape(shape, key.top / (127.0 * kTopSteps));
    }

    const double level = FitEnvelope(zone, shape, ticks_per_second, 0.0);
    zone.hold_ms = shape.held_updates * 1000.0 / ticks_per_second;
    zone.release_ms = kSquaredLineTo100Db * std::max<int>(release, 1) * unit * 1000.0 / ticks_per_second;

    // Each byte counts out of 127, and the volumes' product over 2^13 counts out of 32768.
    const double full = 127.0 * 127.0 * 127.0 * settings.song_volume / static_cast<double>(1 << kVolumeShift) /
                        kFullVoice * (settings.master_volume / kFullVoice);
    zone.gain = level * full * full;

    return zone;
}

} // namespace ultralib::sequence_export::n64
