// SPDX-License-Identifier: MIT

#include "ultralib/sequence_export/n64/input.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/files.h"
#include "xsflib/rip/rom_set.h"

#include "ultralib/machine/rom_set.h"

namespace ultralib::sequence_export::n64
{

namespace
{

// The image of the ROM set at `path`. A warning names the other games whose chips the set holds.
std::vector<uint8_t> ReadSetImage(const std::filesystem::path& path, std::string_view program,
                                  std::vector<std::string>& warnings)
{
    const std::span<const ultralib::machine::Aleck64Set> games = ultralib::machine::Aleck64Sets();
    ultralib::machine::Aleck64SetImage image = ultralib::machine::ReadAleck64Set(path, program, games);
    if (!image.problem.empty())
    {
        throw xsflib::FormatError(image.problem);
    }

    if (!image.others.empty())
    {
        warnings.push_back(xsflib::Utf8(path) + " contains the chips of more than one game. Only " +
                           std::string(games[*image.game].name) +
                           " is converted. Not converted: " + ultralib::machine::OtherGameNames(image, games) +
                           ". Put each game in a folder or an archive of its own.");
    }

    return std::move(image.image);
}

} // namespace

Input::Input(std::unique_ptr<Image> image) : image_(std::move(image))
{
}

Input Input::Open(const std::filesystem::path& path, std::string_view program)
{
    std::optional<std::vector<uint8_t>> file;
    std::vector<std::string> warnings;
    if (xsflib::rip::IsRomSet(path))
    {
        file = ReadSetImage(path, program, warnings);
    }
    else
    {
        file = ntdlib::sequence_export::ReadFile(path);
    }
    if (!file)
    {
        throw std::runtime_error("the file can't be read");
    }

    Input input(std::make_unique<Image>(std::move(*file)));
    input.warnings_ = std::move(warnings);
    input.profile_ = FindProfile(input.image_->Sha1());
    if (input.profile_ != nullptr && input.profile_->streamed_music)
    {
        throw xsflib::FormatError("the game streams its music as waves, with no sequences to convert");
    }

    if (input.profile_ != nullptr && input.profile_->ead != nullptr)
    {
        input.ead_ = EadGame::Open(input.image_->Bytes(), *input.profile_->ead);
    }
    else if (input.profile_ != nullptr && input.profile_->libmus != nullptr)
    {
        input.libmus_ = LibmusGame::Open(input.image_->Bytes(), *input.profile_->libmus);
    }
    else if (input.profile_ != nullptr && input.profile_->sbn != nullptr)
    {
        input.sbn_ = SbnGame::Open(input.image_->Bytes(), *input.profile_->sbn);
    }
    else
    {
        const LibultraProfile* libultra = input.profile_ != nullptr ? &input.profile_->libultra : nullptr;
        input.libultra_ = LibultraGame::Open(input.image_->Bytes(), libultra);
    }

    if (!input.libultra_ && !input.ead_ && !input.libmus_ && !input.sbn_)
    {
        throw xsflib::FormatError("the converter doesn't know this game's sound engine");
    }

    return input;
}

const std::vector<std::string>& Input::Warnings() const
{
    return warnings_;
}

std::vector<ntdlib::sequence_export::Sequence> Input::Sequences() const
{
    if (libmus_)
    {
        return libmus_->Sequences();
    }

    if (sbn_)
    {
        return sbn_->Sequences();
    }

    return ead_ ? ead_->Sequences() : libultra_->Sequences();
}

Conversion Input::Convert(std::size_t index, const PerformOptions& options)
{
    if (libmus_)
    {
        return libmus_->Convert(index, options);
    }

    if (sbn_)
    {
        return sbn_->Convert(index, options);
    }

    return ead_ ? ead_->Convert(index, options) : libultra_->Convert(index, options);
}

} // namespace ultralib::sequence_export::n64
