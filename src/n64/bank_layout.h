// SPDX-License-Identifier: MIT

// What the readers and writers of the N64's banks share: the reading of codebooks, loops and loop states from a bank's
// bytes, and the layout of the bytes that a writer builds.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/bytes.h"

#include "ultralib/n64/bank_file.h"
#include "ultralib/n64/vadpcm.h"

namespace ultralib::n64::bank_layout
{

// A codebook starts with its order and its count of predictors, as two words.
inline constexpr std::size_t kBookHeaderSize = 8;
inline constexpr int32_t kMaxOrder = 16;
inline constexpr int32_t kMaxPredictors = 256;

// A loop of a bank file and of a wave table: its start, end and count, then the VADPCM state of a VADPCM wave.
inline constexpr std::size_t kLoopHeaderSize = 12;

// Reads `count` big-endian coefficients at `at`.
inline std::vector<int16_t> ReadCoefficients(const xsflib::Reader& reader, std::size_t at, std::size_t count)
{
    reader.Sub(at, count * 2);

    std::vector<int16_t> coefficients;
    coefficients.reserve(count);
    for (std::size_t i = 0; i < count; i++)
    {
        coefficients.push_back(reader.S16(at + i * 2));
    }

    return coefficients;
}

// Reads a codebook at `at`: its header, then order * predictors * 8 coefficients. A book with an order or a count of
// predictors out of range throws xsflib::FormatError. `owner` names the file in the message, such as "a bank file".
inline VadpcmBook ReadBook(const xsflib::Reader& reader, std::size_t at, std::string_view owner)
{
    VadpcmBook book;
    book.order = reader.S32(at);
    book.predictor_count = reader.S32(at + 4);
    if (book.order <= 0 || book.order > kMaxOrder || book.predictor_count <= 0 || book.predictor_count > kMaxPredictors)
    {
        throw xsflib::FormatError("a book of " + std::string(owner) + " has an order of " + std::to_string(book.order) +
                                  " and " + std::to_string(book.predictor_count) + " predictors");
    }

    const std::size_t count =
        static_cast<std::size_t>(book.order) * static_cast<std::size_t>(book.predictor_count) * kVadpcmGroupSamples;
    book.coefficients = ReadCoefficients(reader, at + kBookHeaderSize, count);

    return book;
}

// Reads the 16 samples of a VADPCM state at `at`.
inline VadpcmState ReadLoopState(const xsflib::Reader& reader, std::size_t at)
{
    reader.Sub(at, kVadpcmFrameSamples * 2);

    VadpcmState state = {};
    for (std::size_t i = 0; i < kVadpcmFrameSamples; i++)
    {
        state[i] = reader.S16(at + i * 2);
    }

    return state;
}

// Reads a loop of a bank file or a wave table at `at`, with its state when `has_state` is set. The loop's offset stays
// unset.
inline BankLoop ReadLoop(const xsflib::Reader& reader, std::size_t at, bool has_state)
{
    BankLoop loop;
    loop.start = reader.U32(at);
    loop.end = reader.U32(at + 4);
    loop.count = reader.U32(at + 8);
    loop.has_state = has_state;
    if (has_state)
    {
        loop.state = ReadLoopState(reader, at + kLoopHeaderSize);
    }

    return loop;
}

// A bank's bytes as a writer builds them. Offsets count from `base`, and each new object starts on a multiple of
// `alignment` bytes from the start of the bytes.
class Layout
{
public:
    Layout(std::vector<uint8_t> bytes, std::size_t alignment, std::size_t base = 0)
        : bytes_(std::move(bytes)), alignment_(alignment), base_(base)
    {
    }

    // The offset of an object of `size` bytes at the end.
    uint32_t Place(std::size_t size)
    {
        const std::size_t offset = (bytes_.size() + alignment_ - 1) / alignment_ * alignment_;
        bytes_.resize(offset + size, 0);

        return static_cast<uint32_t>(offset - base_);
    }

    // Makes the bytes at least `size` long.
    void Grow(std::size_t size)
    {
        if (bytes_.size() < size)
        {
            bytes_.resize(size, 0);
        }
    }

    // A field of the part before the base.
    void HeaderU32(std::size_t offset, uint32_t value)
    {
        xsflib::StoreBe32(bytes_.data() + offset, value);
    }

    void U8(std::size_t offset, uint8_t value)
    {
        bytes_[base_ + offset] = value;
    }

    void U16(std::size_t offset, uint16_t value)
    {
        xsflib::StoreBe16(bytes_.data() + base_ + offset, value);
    }

    void U32(std::size_t offset, uint32_t value)
    {
        xsflib::StoreBe32(bytes_.data() + base_ + offset, value);
    }

    std::vector<uint8_t> Take()
    {
        return std::move(bytes_);
    }

private:
    std::vector<uint8_t> bytes_;
    std::size_t alignment_;
    std::size_t base_;
};

// The offset that a pointer writes for an index, with 0 for no object.
template <typename Object>
uint32_t OffsetOf(const std::vector<Object>& objects, int32_t index)
{
    return index == kNoObject ? 0 : objects[static_cast<std::size_t>(index)].offset;
}

// Places a codebook at the end when it has no place, and writes its header and coefficients.
inline void WriteBook(Layout& layout, BankBook& book)
{
    if (book.offset == kNoOffset)
    {
        book.offset = layout.Place(kBookHeaderSize + book.book.coefficients.size() * 2);
    }

    layout.U32(book.offset, static_cast<uint32_t>(book.book.order));
    layout.U32(book.offset + 4, static_cast<uint32_t>(book.book.predictor_count));
    for (std::size_t i = 0; i < book.book.coefficients.size(); i++)
    {
        layout.U16(book.offset + kBookHeaderSize + i * 2, static_cast<uint16_t>(book.book.coefficients[i]));
    }
}

} // namespace ultralib::n64::bank_layout
