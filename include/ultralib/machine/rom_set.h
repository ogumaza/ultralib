// SPDX-License-Identifier: MIT

// The ROM sets of Seta's Aleck64 arcade board, a board built on the Nintendo 64. Every chip of a set is stored with
// each pair of bytes reversed. Put together in order, a set's chips make the image of a cartridge. The user gives a set
// as a ZIP archive or as a folder of its chips.

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "xsflib/rip/rom_set.h"

namespace ultralib::machine
{

// A game's ROM set: the game's name, the SHA-1 of the image that its chips make, in lower-case hex, and the chips.
// Every set has its image SHA-1. Sets with the same one are sets of one game.
struct Aleck64Set
{
    std::string_view name;
    std::string_view image_sha1;
    std::span<const xsflib::rip::RomChip> chips;
};

// Every known set. The table is built on the first call. The initialiser of other static data may read it.
std::span<const Aleck64Set> Aleck64Sets();

// The set whose chips make the image with `image_sha1`, or null.
const Aleck64Set* FindAleck64Set(std::string_view image_sha1);

struct Aleck64SetImage
{
    std::vector<uint8_t> image;      // The set's image in the console's byte order, or a ZIP archive's lone image.
    std::optional<std::size_t> game; // The index in `games` of the set whose chips are all here.
    std::vector<std::size_t> others; // With a game: the first index of each other game whose chips are all here.
    std::string problem;             // Without an image: why.
    bool unreadable = false;         // The problem is that the archive or folder can't be read.
};

// Reads the ROM set at `path` and puts together the image of the first of `games` whose chips are all in it. Sets that
// share an image SHA-1 are sets of one game. A ZIP archive that holds no game's chips can hold a cartridge image
// instead: its one entry named .z64, .v64 or .n64, in any case, with its bytes as the entry holds them. `program` names
// the reader in the message about a compression method that it doesn't read.
Aleck64SetImage ReadAleck64Set(const std::filesystem::path& path, std::string_view program,
                               std::span<const Aleck64Set> games = Aleck64Sets());

// The names in `games` of the other games whose chips `image` holds, joined with ", ". `games` is the list that read
// the set.
std::string OtherGameNames(const Aleck64SetImage& image, std::span<const Aleck64Set> games = Aleck64Sets());

} // namespace ultralib::machine
