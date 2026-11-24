// SPDX-License-Identifier: MIT

// Rules of the RSP's audio that several engines' conversions share: the law of an effect send, and the resampler's
// highest pitch.

#pragma once

#include <cmath>
#include <numbers>
#include <vector>

#include "ultralib/sequence_export/n64/instruments.h"

namespace ultralib::sequence_export::n64
{

// The share of the dry level that an effect send of `send` out of 127 leaves, on the sin/cos law.
inline double DryShare(int send)
{
    return std::cos(std::numbers::pi / 2.0 * send / 127.0);
}

// Keys past the resampler's limit play at the limit. Appends the keys of `zone` below `limit_key` as they are, then
// its keys from `limit_key` up at `limit_cents`, whatever the key.
void AddUpToResamplerLimit(std::vector<Zone>& zones, Zone zone, int limit_key, double limit_cents);

} // namespace ultralib::sequence_export::n64
