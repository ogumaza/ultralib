// SPDX-License-Identifier: MIT

// The readers of the N64 Music Tools' pointer banks and songs. The addresses in the comments are places in the code of
// Pokémon Stadium.

#include <cstddef>
#include <cstdint>
#include <span>

#include "xsflib/binary.h"
#include "xsflib/bytes.h"

#include "round_toward_zero.h"
#include "ultralib/n64/mus_player.h"

namespace ultralib::n64
{

namespace
{

// The pointer bank's header.
constexpr std::size_t kBankCount = 0x20;
constexpr std::size_t kBankBaseNotes = 0x24;
constexpr std::size_t kBankDetunes = 0x28;
constexpr std::size_t kBankWaves = 0x2C;
constexpr std::size_t kBankHeaderSize = 0x30;
constexpr std::size_t kDetuneStep = 4;
constexpr std::size_t kWaveSize = 20;
constexpr std::size_t kLoopStateOffset = 12;
constexpr std::size_t kBookCoefficients = 8;
constexpr int kBookBytesPerEntry = 16;
constexpr int kNoteOfNoShift = 48;
constexpr double kCentsPerSemitone = 100.0;

} // namespace

MusBank MusBank::Parse(std::span<const uint8_t> bytes, bool round_toward_zero)
{
    const RoundTowardZero rounding(round_toward_zero);
    if (bytes.size() < kBankHeaderSize)
    {
        throw xsflib::FormatError("a pointer bank is too short for its header");
    }

    const auto word = [&](std::size_t place)
    {
        if (place + 4 > bytes.size())
        {
            throw xsflib::FormatError("a pointer bank's offset lies outside the bank");
        }

        return xsflib::LoadBe32(bytes.data() + place);
    };

    const uint32_t count = word(kBankCount);
    const uint32_t base_notes = word(kBankBaseNotes);
    const uint32_t detunes = word(kBankDetunes);
    const uint32_t waves = word(kBankWaves);
    if (count > bytes.size() / 4 || base_notes + std::size_t{count} > bytes.size() ||
        detunes + std::size_t{count} * kDetuneStep > bytes.size() || waves + std::size_t{count} * 4 > bytes.size())
    {
        throw xsflib::FormatError("a pointer bank's tables lie outside the bank");
    }

    MusBank bank;
    bank.pitches.reserve(count);
    bank.waves.reserve(count);
    for (uint32_t i = 0; i < count; i++)
    {
        // The code at 0x8003BEF0 turns the detune and the base note into one float.
        const auto detune = static_cast<float>(static_cast<int8_t>(bytes[detunes + std::size_t{i} * kDetuneStep]));
        const auto shift =
            static_cast<int8_t>(static_cast<uint8_t>(bytes[base_notes + std::size_t{i}] - kNoteOfNoShift));
        bank.pitches.push_back(static_cast<float>(static_cast<double>(detune) / kCentsPerSemitone) +
                               static_cast<float>(shift));

        const uint32_t place = word(waves + std::size_t{i} * 4);
        if (place + kWaveSize > bytes.size())
        {
            throw xsflib::FormatError("a pointer bank's wave lies outside the bank");
        }

        MusWave wave;
        wave.base = word(place);
        wave.length = word(place + 4);
        wave.type = bytes[place + 8];
        const uint32_t loop = word(place + 12);
        const uint32_t book = word(place + 16);
        if (loop != 0)
        {
            wave.has_loop = true;
            wave.loop_start = word(loop);
            wave.loop_end = word(loop + 4);
            wave.loop_count = word(loop + 8);
            wave.loop_state = loop + static_cast<uint32_t>(kLoopStateOffset);
        }

        if (wave.type == kMusWaveAdpcm)
        {
            if (book == 0)
            {
                throw xsflib::FormatError("a pointer bank's wave has no book");
            }

            const uint32_t order = word(book);
            const uint32_t predictors = word(book + 4);
            wave.book = book + static_cast<uint32_t>(kBookCoefficients);
            wave.book_size = static_cast<int32_t>(order * kBookBytesPerEntry * predictors);
        }

        bank.waves.push_back(wave);
    }

    return bank;
}

MusSong MusSong::Parse(std::span<const uint8_t> bytes, MusSongForm form)
{
    constexpr std::size_t kEarlyHeader = 0x18;
    constexpr std::size_t kLateHeader = 0x2C;
    if (bytes.size() < (form == MusSongForm::kEarly ? kEarlyHeader : kLateHeader))
    {
        throw xsflib::FormatError("a song is too short for its header");
    }

    const auto word = [&](std::size_t place)
    {
        return xsflib::LoadBe32(bytes.data() + place);
    };

    const bool early = form == MusSongForm::kEarly;
    MusSong song;
    song.form = form;
    song.bytes = bytes;
    const uint32_t count = word(early ? 0x00 : 0x04);
    const uint32_t data = word(early ? 0x04 : 0x0C);
    const uint32_t volume = word(early ? 0x08 : 0x10);
    const uint32_t bend = word(early ? 0x0C : 0x14);
    song.envelopes = word(early ? 0x10 : 0x18);
    song.drums = word(early ? 0x14 : 0x1C);
    song.wave_table = early ? 0 : word(0x20);
    song.master_list = early ? 0 : word(0x24);
    if (count > bytes.size() / 4 || data + std::size_t{count} * 4 > bytes.size() ||
        volume + std::size_t{count} * 4 > bytes.size() || bend + std::size_t{count} * 4 > bytes.size())
    {
        throw xsflib::FormatError("a song's lists lie outside its file");
    }

    for (uint32_t i = 0; i < count; i++)
    {
        song.data_lists.push_back(word(data + std::size_t{i} * 4));
        song.volume_lists.push_back(word(volume + std::size_t{i} * 4));
        song.bend_lists.push_back(word(bend + std::size_t{i} * 4));
    }

    return song;
}

} // namespace ultralib::n64
