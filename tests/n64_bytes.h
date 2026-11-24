// SPDX-License-Identifier: MIT

// The builders of the big-endian data that the tests of the Nintendo 64's formats write by hand, and of their standard
// MIDI files.

#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <vector>

#include "seqlib/midi/encoder.h"
#include "xsflib/bytes.h"

namespace ultralib::testing
{

using Bytes = std::vector<uint8_t>;

// Writes a big-endian word over the bytes at `place`.
inline void PutAt(Bytes& out, std::size_t place, uint32_t value)
{
    xsflib::StoreBe32(out.data() + place, value);
}

inline void Append(Bytes& out, std::initializer_list<uint8_t> bytes)
{
    out.insert(out.end(), bytes);
}

// A standard MIDI file of format 0, with 480 ticks to a quarter note.
inline Bytes MakeSong(std::initializer_list<uint8_t> track)
{
    constexpr uint16_t kDivision = 480;
    Bytes file;
    seqlib::midi::AppendHeader(file, 0, 1, kDivision);
    seqlib::midi::AppendTrack(file, std::span<const uint8_t>(track.begin(), track.size()));

    return file;
}

} // namespace ultralib::testing
