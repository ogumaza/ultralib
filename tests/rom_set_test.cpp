// SPDX-License-Identifier: MIT

// The ROM sets of the Aleck64: the table of known sets, and the reading of a set into an image, on synthetic chips in a
// folder and in ZIP archives from xsflib/testing/zip_archive.h.

#include "ultralib/machine/rom_set.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "xsflib/files.h"
#include "xsflib/sha1.h"
#include "xsflib/testing/check.h"
#include "xsflib/testing/temp_folder.h"
#include "xsflib/testing/zip_archive.h"

namespace
{

using ultralib::machine::Aleck64Set;
using ultralib::machine::Aleck64SetImage;
using xsflib::rip::RomChip;
using xsflib::testing::StoredEntry;
using xsflib::testing::ZipArchive;

constexpr char kProgram[] = "the test";

// Three games of synthetic chips, each pair of bytes reversed as the sets store them. Game A has two chips, game B
// one. Game C needs a chip that no file holds.
const std::vector<uint8_t> kLowChip = {0x80, 0x37, 0x12, 0x40};
const std::vector<uint8_t> kHighChip = {0x02, 0x01, 0x04, 0x03};
const std::vector<uint8_t> kOtherChip = {0x0B, 0x0A, 0x0D, 0x0C, 0x0F, 0x0E};
const std::vector<uint8_t> kMissingChip = {9, 9, 9, 9, 9, 9, 9, 9};

std::string Sha1(const std::vector<uint8_t>& data)
{
    return xsflib::Sha1::Hex(xsflib::Sha1::Of(data));
}

RomChip Chip(const std::vector<uint8_t>& data, uint32_t offset)
{
    return {Sha1(data), static_cast<uint32_t>(data.size()), offset, true};
}

const std::vector<RomChip> kChipsA = {Chip(kLowChip, 0), Chip(kHighChip, 4)};
const std::vector<RomChip> kChipsB = {Chip(kOtherChip, 0)};
const std::vector<RomChip> kChipsC = {Chip(kOtherChip, 0), Chip(kMissingChip, 6)};

const std::vector<Aleck64Set> kGames = {
    {"Game C", "cccc", kChipsC},
    {"Game A", "aaaa", kChipsA},
    {"Game A, another set", "aaaa", kChipsA},
    {"Game B", "bbbb", kChipsB},
};

const std::vector<uint8_t> kImageA = {0x37, 0x80, 0x40, 0x12, 0x01, 0x02, 0x03, 0x04};

void Write(const std::filesystem::path& path, const std::vector<uint8_t>& data)
{
    XSFLIB_REQUIRE(!xsflib::WriteFile(path, data));
}

void KnownSets()
{
    std::set<std::string_view> images;
    for (const Aleck64Set& set : ultralib::machine::Aleck64Sets())
    {
        XSFLIB_CHECK(!set.name.empty() && set.image_sha1.size() == 40 && !set.chips.empty());
        XSFLIB_CHECK(images.insert(set.image_sha1).second);
        XSFLIB_CHECK(ultralib::machine::FindAleck64Set(set.image_sha1) == &set);
    }

    XSFLIB_CHECK(images.size() == 11);

    const Aleck64Set* eleven_beat = ultralib::machine::FindAleck64Set("5432056f34c378b5862009e5b79a256c10c5a20e");
    XSFLIB_REQUIRE(eleven_beat != nullptr);
    XSFLIB_CHECK(eleven_beat->name == "Eleven Beat");
    XSFLIB_CHECK(eleven_beat->chips.size() == 1 && eleven_beat->chips[0].size == 0x800000);
    XSFLIB_CHECK(ultralib::machine::FindAleck64Set("0000000000000000000000000000000000000000") == nullptr);
}

// A folder with every chip of games A and B: A comes first, and B is the other game. A's second set has A's image, so
// it isn't another game.
void TwoGames()
{
    const xsflib::testing::TempFolder folder("ultralib_rom_set_two");
    Write(folder.Path() / "low", kLowChip);
    Write(folder.Path() / "high", kHighChip);
    Write(folder.Path() / "other", kOtherChip);

    const Aleck64SetImage image = ultralib::machine::ReadAleck64Set(folder.Path(), kProgram, kGames);

    XSFLIB_CHECK(image.problem.empty() && !image.unreadable);
    XSFLIB_CHECK(image.game == std::size_t{1});
    XSFLIB_CHECK(image.image == kImageA);
    XSFLIB_CHECK(image.others == std::vector<std::size_t>{3});
}

// One game's chips, with a copy of a chip under another name: no other game.
void OneGame()
{
    const xsflib::testing::TempFolder folder("ultralib_rom_set_one");
    Write(folder.Path() / "low", kLowChip);
    Write(folder.Path() / "low copy", kLowChip);
    Write(folder.Path() / "high", kHighChip);

    const Aleck64SetImage image = ultralib::machine::ReadAleck64Set(folder.Path(), kProgram, kGames);

    XSFLIB_CHECK(image.game == std::size_t{1});
    XSFLIB_CHECK(image.image == kImageA);
    XSFLIB_CHECK(image.others.empty());
}

// A set with some of a game's chips names the chips that it's missing. A cartridge image in a folder is never read in
// its place.
void PartialSet()
{
    const xsflib::testing::TempFolder folder("ultralib_rom_set_partial");
    Write(folder.Path() / "low", kLowChip);
    Write(folder.Path() / "game.z64", kImageA);

    const Aleck64SetImage image = ultralib::machine::ReadAleck64Set(folder.Path(), kProgram, kGames);

    XSFLIB_CHECK(!image.game && image.image.empty() && !image.unreadable);
    XSFLIB_CHECK(image.problem ==
                 "the set of Game A is missing 1 of its 2 chips: 4 bytes with SHA-1 " + Sha1(kHighChip));
}

// A ZIP archive with no game's chips gives its one cartridge image. With two images, it gives none.
void ZipImage()
{
    const xsflib::testing::TempFolder folder("ultralib_rom_set_zip");
    const std::vector<uint8_t> cartridge = {0x80, 0x37, 0x12, 0x40, 1, 2, 3, 4, 5};
    Write(folder.Path() / "one.zip",
          ZipArchive({StoredEntry("readme.txt", {'h', 'i'}), StoredEntry("Game.V64", cartridge)}));
    Write(folder.Path() / "two.zip", ZipArchive({StoredEntry("a.z64", cartridge), StoredEntry("b.n64", cartridge)}));
    Write(folder.Path() / "chips.zip", ZipArchive({StoredEntry("x", kOtherChip), StoredEntry("game.z64", cartridge)}));

    const Aleck64SetImage one = ultralib::machine::ReadAleck64Set(folder.Path() / "one.zip", kProgram, kGames);
    const Aleck64SetImage two = ultralib::machine::ReadAleck64Set(folder.Path() / "two.zip", kProgram, kGames);
    const Aleck64SetImage chips = ultralib::machine::ReadAleck64Set(folder.Path() / "chips.zip", kProgram, kGames);

    XSFLIB_CHECK(!one.game && one.image == cartridge && one.problem.empty());
    XSFLIB_CHECK(!two.game && two.image.empty() && two.problem == "no profile's chips are in the set");
    XSFLIB_CHECK(chips.game == std::size_t{3} &&
                 chips.image == std::vector<uint8_t>({0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F}));
}

void Unreadable()
{
    const xsflib::testing::TempFolder folder("ultralib_rom_set_unreadable");
    Write(folder.Path() / "bad.zip", {'P', 'K', 3, 4, 0});

    const Aleck64SetImage image = ultralib::machine::ReadAleck64Set(folder.Path() / "bad.zip", kProgram, kGames);

    XSFLIB_CHECK(image.unreadable && !image.problem.empty() && image.image.empty() && !image.game);
}

} // namespace

int main()
{
    KnownSets();
    TwoGames();
    OneGame();
    PartialSet();
    ZipImage();
    Unreadable();

    return xsflib::testing::Summary("rom_set");
}
