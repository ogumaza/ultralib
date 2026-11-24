// SPDX-License-Identifier: MIT

#include "rsp_audio.h"

#include <algorithm>
#include <utility>

namespace ultralib::sequence_export::n64
{

void AddUpToResamplerLimit(std::vector<Zone>& zones, Zone zone, int limit_key, double limit_cents)
{
    if (zone.low_key < limit_key)
    {
        Zone below = zone;
        below.high_key = std::min(zone.high_key, limit_key - 1);
        zones.push_back(below);
    }

    if (zone.high_key >= limit_key)
    {
        zone.low_key = std::max(zone.low_key, limit_key);
        zone.follows_key = false;
        zone.tune_cents = limit_cents;
        zones.push_back(std::move(zone));
    }
}

} // namespace ultralib::sequence_export::n64
