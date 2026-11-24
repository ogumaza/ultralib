// SPDX-License-Identifier: MIT

#include "ultralib/machine/rom_set.h"

#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <system_error>
#include <utility>

#include "xsflib/sha1.h"

namespace ultralib::machine
{

namespace
{

using xsflib::rip::RomChip;
using xsflib::rip::SetFile;

// The extensions of a cartridge image's file, in any case.
constexpr std::array<std::string_view, 3> kImageExtensions = {".z64", ".v64", ".n64"};

// A chip by its size and SHA-1.
using ChipKey = std::pair<uint64_t, std::string>;

// Whether every chip of `set` is among `chips`. A set with no chips never is.
bool Complete(const Aleck64Set& set, const std::map<ChipKey, std::size_t>& chips)
{
    const auto here = [&chips](const RomChip& chip)
    {
        return chips.count({chip.size, chip.sha1}) != 0;
    };

    return !set.chips.empty() && std::ranges::all_of(set.chips, here);
}

// The image of `set`. Each chip of the set is in `files`, at the index that `chips` gives. Only the set's chips go to
// xsflib's loader. The loader hashes each file that it's given.
std::vector<uint8_t> SetImage(const Aleck64Set& set, std::vector<SetFile>& files,
                              const std::map<ChipKey, std::size_t>& chips)
{
    std::set<std::size_t> used;
    for (const RomChip& chip : set.chips)
    {
        used.insert(chips.at({chip.size, chip.sha1}));
    }

    std::vector<SetFile> set_files;
    for (std::size_t index : used)
    {
        set_files.push_back(std::move(files[index]));
    }

    const xsflib::rip::RomSetGame game = {set.name, set.chips};

    return std::move(xsflib::rip::BuildRomSetImage({&game, 1}, set_files).rom);
}

} // namespace

std::span<const Aleck64Set> Aleck64Sets()
{
    static const std::array kElevenBeatChips = {RomChip{"824911452639cedf6a8186c05cd046e61fc98896", 0x800000, 0, true}};
    static const std::array kHiPaiParadiseChips = {
        RomChip{"e909ea5b71b81087da07821c4f57244576363678", 0x1000000, 0, true},
        RomChip{"7c3e59f6520dc3f0aa592e682fa82e30ffd1f4d0", 0x1000000, 0x1000000, true}};
    static const std::array kHiPaiParadise2Chips = {
        RomChip{"279a006436069232383b2065e7416d8ccc76e3ca", 0x1000000, 0, true},
        RomChip{"a6d2d805f96f0ad7294ebf41b13573320154b2a4", 0x1000000, 0x1000000, true}};
    static const std::array kTowerAndShaftChips = {
        RomChip{"7dfa3447d2c489c0448c4004dc12d3037c05a0f3", 0x1000000, 0, true}};
    static const std::array kHanabiDeDoonChips = {
        RomChip{"4f41ee23edc18110be1218ba333d1c58376ab175", 0x1000000, 0, true},
        RomChip{"c8cc6c0a456b593aef711d0a75b2342ba2f8203f", 0x1000000, 0x1000000, true}};
    static const std::array kKurukuruFeverChips = {
        RomChip{"824fdce01fffdfcbcc9b1fbda4ab389a10b2b418", 0x1000000, 0, true},
        RomChip{"2b6ca1a769dee74e112c2b287dacd0bf46dda091", 0x1000000, 0x1000000, true}};
    static const std::array kMagicalTetrisChallengeChips = {
        RomChip{"59932c70b43ff8e9264c670f37b3abbe939b7f95", 0x1000000, 0, true}};
    static const std::array kMayjinsen3Chips = {RomChip{"b5834bfde5b8a7e20415b2593abd76ec95ab27c7", 0x800000, 0, true}};
    static const std::array kStarSoldierChips = {
        RomChip{"3794606c008fb69f5d16dcccece94d03da23bf8a", 0xC00000, 0, true}};
    static const std::array kSuperRealMahjongVsChips = {
        RomChip{"a78de955f2fcd99dda14e782984368b320eb5415", 0x2000000, 0, true}};
    static const std::array kVividDollsChips = {RomChip{"06d8b808cc19378b046803f4dc75c7d791b7767f", 0x800000, 0, true}};

    static const std::array kSets = {
        Aleck64Set{"Eleven Beat", "5432056f34c378b5862009e5b79a256c10c5a20e", kElevenBeatChips},
        Aleck64Set{"Mayjinsen 3", "da401fc96a2b69db81970d879a2255e54db88e34", kMayjinsen3Chips},
        Aleck64Set{"Star Soldier: Vanishing Earth", "a29348632ed7726d75d693dfb007f277c1f6a563", kStarSoldierChips},
        Aleck64Set{"Super Real Mahjong VS", "a590216a171a61517cd07ff629d8215631c8be87", kSuperRealMahjongVsChips},
        Aleck64Set{"Vivid Dolls", "747e9420cce23ff82b5922cd91ec43272d03fa4f", kVividDollsChips},
        Aleck64Set{"Hi Pai Paradise", "68329c94e1b9342ee60713e9e0945ea2663210c7", kHiPaiParadiseChips},
        Aleck64Set{"Tower & Shaft", "ba4474dc4967adb2a41cf3b0022c6215990c0e64", kTowerAndShaftChips},
        Aleck64Set{"Hanabi de Doon! - Don-chan Puzzle", "2a77e47d34b8d9d89eb37ffc062e9ec0c2121abc", kHanabiDeDoonChips},
        Aleck64Set{"Hi Pai Paradise 2", "47fdc89620ddf68972e861dee1536a26a309e35c", kHiPaiParadise2Chips},
        Aleck64Set{"Kurukuru Fever", "f59c121e4d4c2c57c2ea4728919775a8d5d5e02e", kKurukuruFeverChips},
        Aleck64Set{"Magical Tetris Challenge", "7e357995def4078a54ba8e4486f361d631ff3329",
                   kMagicalTetrisChallengeChips},
    };

    return kSets;
}

const Aleck64Set* FindAleck64Set(std::string_view image_sha1)
{
    const std::span<const Aleck64Set> sets = Aleck64Sets();
    const auto found = std::ranges::find(sets, image_sha1, &Aleck64Set::image_sha1);

    return found != sets.end() ? &*found : nullptr;
}

Aleck64SetImage ReadAleck64Set(const std::filesystem::path& path, std::string_view program,
                               std::span<const Aleck64Set> games)
{
    std::vector<xsflib::rip::RomSetGame> loader_games;
    loader_games.reserve(games.size());
    for (const Aleck64Set& set : games)
    {
        loader_games.push_back({set.name, set.chips});
    }

    Aleck64SetImage result;
    std::vector<SetFile> files;
    if (std::optional<std::string> problem = xsflib::rip::ReadRomSet(path, loader_games, program, files))
    {
        result.problem = std::move(*problem);
        result.unreadable = true;
        return result;
    }

    std::map<ChipKey, std::size_t> chips;
    for (std::size_t index = 0; index < files.size(); index++)
    {
        chips.emplace(ChipKey{files[index].data.size(), xsflib::Sha1::Hex(xsflib::Sha1::Of(files[index].data))}, index);
    }

    std::set<std::string_view> images;
    for (std::size_t index = 0; index < games.size(); index++)
    {
        const Aleck64Set& set = games[index];
        if (images.count(set.image_sha1) != 0 || !Complete(set, chips))
        {
            continue;
        }

        images.insert(set.image_sha1);
        if (result.game)
        {
            result.others.push_back(index);
        }
        else
        {
            result.game = index;
        }
    }

    if (result.game)
    {
        result.image = SetImage(games[*result.game], files, chips);
        return result;
    }

    // A ZIP archive with a cartridge image and no game's chips. A set with some of a game's chips keeps its message
    // about the missing ones, unless it holds an image too.
    std::error_code error;
    SetFile* lone = xsflib::rip::OnlyFileWithExtension(files, kImageExtensions);
    if (lone != nullptr && !std::filesystem::is_directory(path, error))
    {
        result.image = std::move(lone->data);
        return result;
    }

    result.problem = xsflib::rip::BuildRomSetImage(loader_games, files).problem;

    return result;
}

std::string OtherGameNames(const Aleck64SetImage& image, std::span<const Aleck64Set> games)
{
    std::string names;
    for (const std::size_t other : image.others)
    {
        names += (names.empty() ? "" : ", ") + std::string(games[other].name);
    }

    return names;
}

} // namespace ultralib::machine
