// SPDX-License-Identifier: MIT

#include <algorithm>
#include <cstdio>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/inflate.h"
#include "ntdlib/yay0.h"
#include "xsflib/binary.h"
#include "xsflib/bytes.h"

#include "libmus_shared.h"
#include "n64/bank_layout.h"
#include "ultralib/n64/bank_file.h"
#include "ultralib/n64/vadpcm.h"
#include "ultralib/sequence_export/n64/libmus.h"
#include "ultralib/sequence_export/n64/wave.h"

namespace ultralib::sequence_export::n64
{

namespace
{

namespace n64 = ultralib::n64;
using libmus::kTopSteps;

} // namespace

std::vector<uint8_t> UnpackDirectoryFile(std::span<const uint8_t> file)
{
    constexpr std::size_t kHeaderSize = 4;
    constexpr uint8_t kPacked = 1;
    constexpr int kItemsInGroup = 8;
    constexpr std::size_t kLongLength = 0x11;
    if (file.size() < kHeaderSize || file[0] != kPacked)
    {
        throw xsflib::FormatError("a file of the directory isn't packed");
    }

    std::size_t at = kHeaderSize;
    const auto next = [&]() -> uint8_t
    {
        if (at >= file.size())
        {
            throw xsflib::FormatError("a packed file stops before its end");
        }

        return file[at++];
    };

    const std::size_t size = std::size_t{file[1]} << 16 | std::size_t{file[2]} << 8 | file[3];
    std::vector<uint8_t> out;
    out.reserve(size);
    while (true)
    {
        const uint8_t flags = next();
        for (int item = kItemsInGroup - 1; item >= 0; item--)
        {
            if (((flags >> item) & 1) == 0)
            {
                out.push_back(next());
                continue;
            }

            const uint8_t first = next();
            const uint8_t second = next();
            const std::size_t distance = std::size_t{first & 0xF0U} << 4 | second;
            if (distance == 0)
            {
                if (out.size() != size)
                {
                    throw xsflib::FormatError("a packed file doesn't hold the size that it gives");
                }

                return out;
            }

            if (distance > out.size())
            {
                throw xsflib::FormatError("a packed file copies from before its start");
            }

            const std::size_t nibble = first & 0x0FU;
            const std::size_t length = nibble != 0 ? nibble + 1 : next() + kLongLength;
            for (std::size_t i = 0; i < length; i++)
            {
                const uint8_t copied = out[out.size() - distance];
                out.push_back(copied);
            }
        }
    }
}

std::unique_ptr<LibmusGame> LibmusGame::Open(std::span<const uint8_t> image, const LibmusProfile& profile)
{
    std::unique_ptr<LibmusGame> game(new LibmusGame);
    game->image_ = image;
    game->profile_ = &profile;

    // A word or a halfword of a table that the profile places in the image.
    const auto table_word = [&image](std::size_t at, std::size_t size)
    {
        if (at > image.size() || image.size() - at < size)
        {
            throw xsflib::FormatError("a table of the game's profile isn't in the image");
        }

        return size == 2 ? xsflib::LoadBe16(image.data() + at) : xsflib::LoadBe32(image.data() + at);
    };

    // The game's bank and its songs, compressed, in a directory: the places of the wave table and the samples, the
    // count of songs and the place of each.
    std::optional<Bank> directory_bank;
    if (profile.directory != 0)
    {
        constexpr std::size_t kSongPlaces = 12;

        const auto word = [&](std::size_t at)
        {
            return table_word(profile.directory + at, 4);
        };

        const auto from = [&](std::size_t at)
        {
            const std::size_t place = profile.directory + word(at);
            if (place > image.size())
            {
                throw xsflib::FormatError("a part of the game's directory isn't in the image");
            }

            return image.subspan(place);
        };

        directory_bank.emplace();
        directory_bank->unpacked_pointers = ntdlib::DecodeYay0(from(0));
        directory_bank->pointers = directory_bank->unpacked_pointers;
        directory_bank->samples = from(4);

        const uint32_t count = word(8);
        game->unpacked_songs_.reserve(count);
        for (uint32_t i = 0; i < count; i++)
        {
            game->unpacked_songs_.push_back(ntdlib::DecodeYay0(from(kSongPlaces + std::size_t{4} * i)));
            game->songs_.push_back(game->unpacked_songs_.back());
        }
    }

    const LibmusFileTable& files = profile.cartridge_files;
    if (files.table != 0)
    {
        constexpr std::size_t kFileEntry = 8;

        // The numbers below the first song name no song.
        game->songs_.resize(files.first_song);
        for (std::size_t i = 0; i < profile.song_count; i++)
        {
            const std::size_t at =
                files.table + kFileEntry * table_word(files.song_files + std::size_t{2} * (files.first_song + i), 2);
            if (at + kFileEntry > image.size())
            {
                throw xsflib::FormatError("a song's file isn't in the game's table of files");
            }

            const std::size_t start = files.base + xsflib::LoadBe32(image.data() + at);
            const std::size_t size = xsflib::LoadBe32(image.data() + at + 4);
            if (size == 0 || start + size > image.size())
            {
                throw xsflib::FormatError("a song isn't in the image");
            }

            game->songs_.push_back(image.subspan(start, size));
        }
    }

    // Each entry of the table holds the start and the end of a song, or only its start if the ends are in a separate
    // table.
    const std::size_t stride = profile.song_ends != 0 ? 4 : 8;
    for (std::size_t i = 0; files.table == 0 && i < profile.song_count; i++)
    {
        const std::size_t at = profile.songs + i * stride;
        const std::size_t end_at = profile.song_ends != 0 ? profile.song_ends + i * 4 : at + 4;
        const uint32_t start = table_word(at, 4);
        const uint32_t end = table_word(end_at, 4);
        if (end <= start || end > image.size())
        {
            throw xsflib::FormatError("a song isn't in the image");
        }

        game->songs_.push_back(image.subspan(start, end - start));
    }

    // The profile places each song, and gives its name.
    for (const LibmusSong& song : profile.song_list)
    {
        if (song.end <= song.start || song.end > image.size())
        {
            throw xsflib::FormatError("a song isn't in the image");
        }

        const std::span<const uint8_t> file = image.subspan(song.start, song.end - song.start);
        if (song.deflated)
        {
            std::vector<uint8_t> unpacked;
            std::size_t used = 0;
            std::string error;
            if (!ntdlib::DecodeDeflate(file, unpacked, used, error))
            {
                throw xsflib::FormatError("a song's DEFLATE stream is corrupt: " + error);
            }

            game->unpacked_songs_.push_back(std::move(unpacked));
            game->songs_.push_back(game->unpacked_songs_.back());
        }
        else
        {
            game->songs_.push_back(file);
        }
    }

    // The game's table of songs names a packed file and a name for each.
    if (profile.files.directory != 0)
    {
        constexpr std::size_t kDirectoryEntry = 8;
        constexpr std::size_t kTableEntry = 8;

        const auto unpack = [&](std::size_t number)
        {
            const std::size_t at = profile.files.directory + number * kDirectoryEntry;
            const uint32_t start = table_word(at, 4);
            const uint32_t end = table_word(at + 4, 4);
            if (end <= start || end > image.size())
            {
                throw xsflib::FormatError("a file isn't in the image");
            }

            return UnpackDirectoryFile(image.subspan(start, end - start));
        };

        const std::vector<uint8_t> table_file = unpack(profile.files.table_file);
        for (std::size_t i = 0; i < profile.files.table_count; i++)
        {
            const std::size_t at = profile.files.table + i * kTableEntry;
            if (at + kTableEntry > table_file.size())
            {
                throw xsflib::FormatError("the game's table of songs isn't in its file");
            }

            game->unpacked_songs_.push_back(unpack(xsflib::LoadBe32(table_file.data() + at)));
            game->songs_.push_back(game->unpacked_songs_.back());

            const std::size_t name = xsflib::LoadBe32(table_file.data() + at + 4) - profile.files.file_address;
            if (name >= table_file.size())
            {
                throw xsflib::FormatError("a song's name isn't in the table's file");
            }

            const std::span<const uint8_t> rest = std::span(table_file).subspan(name);
            game->names_.emplace_back(rest.begin(), std::ranges::find(rest, uint8_t{0}));
        }
    }

    const auto read_waves = [&](Bank& bank)
    {
        // The table's header: a name, flags, the name of the sample file, the count, and three lists.
        constexpr std::size_t kCount = 0x20;
        constexpr std::size_t kBaseNotes = 0x24;
        constexpr std::size_t kDetunes = 0x28;
        constexpr std::size_t kWaveList = 0x2C;
        constexpr int kBaseOffset = 48;

        const std::span<const uint8_t> pointers = bank.pointers;
        const auto word = [&](std::size_t at)
        {
            return xsflib::LoadBe32(pointers.data() + at);
        };

        const uint32_t count = word(kCount);
        for (uint32_t i = 0; i < count; i++)
        {
            const uint32_t wave = word(word(kWaveList) + std::size_t{4} * i);
            BankWave entry;
            entry.base = word(wave);
            entry.length = word(wave + std::size_t{4});
            entry.vadpcm = pointers[wave + std::size_t{8}] == 0;
            entry.loop = word(wave + std::size_t{12});
            entry.book = word(wave + std::size_t{16});
            bank.waves.push_back(entry);

            const int cents = static_cast<int8_t>(pointers[word(kDetunes) + std::size_t{4} * i]);
            const int base = static_cast<int8_t>(static_cast<uint8_t>(pointers[word(kBaseNotes) + i] - kBaseOffset));
            bank.detunes.push_back(cents / 100.0 + base);
        }
    };

    if (directory_bank)
    {
        read_waves(*directory_bank);
        game->banks_.push_back(std::move(*directory_bank));
    }

    for (const LibmusBankPlace& place : profile.banks)
    {
        Bank bank;
        bank.first_song = place.first_song;
        // The table of waves ends where the samples start, if they follow it.
        bank.pointers = place.samples > place.pointers ? image.subspan(place.pointers, place.samples - place.pointers)
                                                       : image.subspan(place.pointers);
        bank.samples = image.subspan(place.samples);
        read_waves(bank);
        game->banks_.push_back(std::move(bank));
    }

    game->repeats_.assign(game->songs_.size(), false);
    for (std::size_t later = 1; later < game->songs_.size(); later++)
    {
        for (std::size_t earlier = 0; earlier < later && !game->repeats_[later]; earlier++)
        {
            game->repeats_[later] = game->Repeats(later, earlier);
        }
    }

    return game;
}

std::vector<ntdlib::sequence_export::Sequence> LibmusGame::Sequences() const
{
    std::vector<ntdlib::sequence_export::Sequence> sequences;
    for (std::size_t i = 0; i < songs_.size(); i++)
    {
        if (!repeats_[i] && !songs_[i].empty())
        {
            sequences.push_back({i, NameOf(i)});
        }
    }

    std::size_t beside = songs_.size();
    for (const LibmusTempoChange& change : profile_->tempo_changes)
    {
        if (change.beside)
        {
            sequences.push_back({beside++, NameOf(change.song, LibmusTempoSettings(change))});
        }
    }

    return sequences;
}

std::string LibmusGame::NameOf(std::size_t index, const std::string& settings) const
{
    if (index < names_.size())
    {
        return SequenceLabel(index, settings.empty() ? names_[index] : names_[index] + ", " + settings);
    }

    return SequenceLabel(index, settings);
}

// A song repeats an earlier one if the two hold the same data, take the same bank and have the same entry in the game's
// table of volumes.
bool LibmusGame::Repeats(std::size_t later, std::size_t earlier)
{
    if (!std::ranges::equal(songs_[later], songs_[earlier]) || &BankOfSong(later) != &BankOfSong(earlier))
    {
        return false;
    }

    if (!profile_->song_list.empty())
    {
        const LibmusSong& a = profile_->song_list[later];
        const LibmusSong& b = profile_->song_list[earlier];
        return a.volume == b.volume && a.reverb_base == b.reverb_base;
    }

    if (!profile_->song_volume_list.empty())
    {
        return profile_->song_volume_list[later] == profile_->song_volume_list[earlier];
    }

    if (profile_->song_volumes == 0)
    {
        return true;
    }

    const auto entry = [&](std::size_t song)
    {
        return image_.subspan(profile_->song_volumes + song * profile_->song_volume_stride,
                              profile_->song_volume_stride);
    };

    return std::ranges::equal(entry(later), entry(earlier));
}

std::size_t LibmusGame::SongCount() const
{
    return songs_.size();
}

std::span<const uint8_t> LibmusGame::SongFile(std::size_t number) const
{
    return songs_.at(number);
}

std::span<const uint8_t> LibmusGame::WaveTable(std::size_t number) const
{
    return BankOfSong(number).pointers;
}

std::size_t LibmusGame::SamplePlace(std::size_t number) const
{
    return static_cast<std::size_t>(BankOfSong(number).samples.data() - image_.data());
}

LibmusGame::Bank& LibmusGame::BankOfSong(std::size_t index)
{
    return const_cast<Bank&>(std::as_const(*this).BankOfSong(index));
}

const LibmusGame::Bank& LibmusGame::BankOfSong(std::size_t index) const
{
    const Bank* found = &banks_.at(0);
    for (const Bank& bank : banks_)
    {
        if (bank.first_song <= index)
        {
            found = &bank;
        }
    }

    return *found;
}

std::shared_ptr<const Wave> LibmusGame::WaveAt(Bank& bank, int32_t index) const
{
    const auto found = bank.decoded.find(index);
    if (found != bank.decoded.end())
    {
        return found->second;
    }

    // The table's books and loops have the form of a bank file's.
    const BankWave& wave = bank.waves.at(static_cast<std::size_t>(index));
    const xsflib::Reader pointers(bank.pointers, true);
    const n64::VadpcmBook book =
        wave.vadpcm ? n64::bank_layout::ReadBook(pointers, wave.book, "a wave table") : n64::VadpcmBook{};
    const n64::BankLoop loop =
        wave.loop != 0 ? n64::bank_layout::ReadLoop(pointers, wave.loop, wave.vadpcm) : n64::BankLoop{};

    char name[32];
    std::snprintf(name, sizeof(name), "w%08X", wave.base);
    std::shared_ptr<const Wave> out =
        DecodeWave(bank.samples.subspan(wave.base, wave.length), wave.vadpcm ? &book : nullptr,
                   wave.loop != 0 ? &loop : nullptr, name, profile_->output_rate);
    bank.decoded[index] = out;

    return out;
}

Conversion LibmusGame::Convert(std::size_t index, const PerformOptions& options)
{
    // A version that the game's code plays beside the song comes after the songs. Any other change replaces the song.
    const LibmusTempoChange* change = nullptr;
    std::size_t beside = songs_.size();
    for (const LibmusTempoChange& candidate : profile_->tempo_changes)
    {
        if (candidate.beside ? beside++ == index : candidate.song == index)
        {
            change = &candidate;
        }
    }

    const std::string name =
        change != nullptr && change->beside ? NameOf(change->song, LibmusTempoSettings(*change)) : NameOf(index);

    if (change != nullptr)
    {
        index = change->song;
    }

    LibmusSettings settings;
    if (change != nullptr)
    {
        settings.tempo_scale = change->scale;
    }

    if (!profile_->song_list.empty())
    {
        settings.song_volume = profile_->song_list[index].volume;
        settings.reverb_base = profile_->song_list[index].reverb_base;
    }
    else if (profile_->song_volume != 0)
    {
        settings.song_volume = profile_->song_volume;
    }
    else
    {
        settings.song_volume = profile_->song_volume_list.empty()
                                   ? image_[profile_->song_volumes + index * profile_->song_volume_stride + 1]
                                   : profile_->song_volume_list[index];
    }

    settings.master_volume = profile_->master_volume;
    settings.output_rate = profile_->output_rate;
    settings.early_form = profile_->early_form;
    settings.early_player = profile_->early_player;
    settings.clipped_envelopes = profile_->clipped_envelopes;
    settings.loop_on_later_passes = profile_->loop_on_later_passes;

    Bank& bank = BankOfSong(index);
    LibmusPerformance played = PerformLibmus(songs_.at(index), bank.detunes, settings, options);
    for (const LibmusPresetKey& key : played.presets)
    {
        // A clipped preset's name gives the envelope's value at the stop, ahead of the bytes that a SoundFont's 19
        // characters may cut.
        char top[16] = "";
        if (key.top != 0)
        {
            std::snprintf(top, sizeof(top), " top%d", key.top / kTopSteps);
        }

        char preset_name[48];
        std::snprintf(preset_name, sizeof(preset_name), "w%d%s e%d-%d-%d-%d-%d", key.wave, top, key.envelope[1],
                      key.envelope[3], key.envelope[5], key.envelope[4], key.envelope[6]);

        Zone zone =
            LibmusZone(key, bank.detunes.at(static_cast<std::size_t>(key.wave)), settings, played.ticks_per_second);
        zone.wave = WaveAt(bank, key.wave);

        Instrument preset;
        preset.name = preset_name;
        preset.zones.push_back(std::move(zone));
        played.performance.presets.push_back(std::move(preset));
    }

    return BuildFiles(std::move(played.performance), name, options.program, options.resampler_filter);
}

} // namespace ultralib::sequence_export::n64
