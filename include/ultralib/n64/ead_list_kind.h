// SPDX-License-Identifier: MIT

// The kinds of list of voices in the EAD engines. Each pool of voices has one list of each kind, and the heads of a
// pool's lists come in this order.

#pragma once

#include <cstdint>

namespace ultralib::n64
{

enum class EadListKind : uint8_t
{
    kDisabled,
    kDecaying,
    kReleasing,
    kActive,
};

} // namespace ultralib::n64
