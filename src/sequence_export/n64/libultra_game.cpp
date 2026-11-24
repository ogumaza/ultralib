// SPDX-License-Identifier: MIT

#include <algorithm>
#include <cstdio>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/bytes.h"

#include "ultralib/n64/rom_data.h"
#include "ultralib/n64/seq_file.h"
#include "ultralib/sequence_export/n64/libultra.h"

namespace ultralib::sequence_export::n64
{

namespace
{

namespace n64 = ultralib::n64;

// A standard MIDI file's header, and each track's.
constexpr std::size_t kMidiHeaderSize = 14;
constexpr std::size_t kMidiTrackHeaderSize = 8;

std::string HexName(uint32_t value)
{
    char name[16];
    std::snprintf(name, sizeof(name), "w%08X", value);

    return name;
}

// The settings of a song of set `set`, for its label. Set 0 holds the profile's songs, and the label counts the sets
// from 1: the first of the profile's other sets is "set 2".
std::string SetSettings(std::size_t set)
{
    return "set " + std::to_string(set + 1);
}

// Takes a track's events from a compressed MIDI file or a standard one.
LibultraSong ReadSong(std::span<const uint8_t> bytes)
{
    LibultraSong song;
    if (n64::CompressedMidi::Matches(bytes))
    {
        const n64::CompressedMidi file = n64::CompressedMidi::Parse(bytes);
        song.division = file.Division();
        for (std::size_t track = 0; track < n64::CompressedMidi::kTrackCount; track++)
        {
            song.tracks.push_back(file.DecodeTrack(track));
        }
    }
    else
    {
        const n64::StandardMidi file = n64::StandardMidi::Parse(bytes);
        song.division = file.Division();
        for (std::size_t track = 0; track < file.TrackCount(); track++)
        {
            song.tracks.push_back(file.DecodeTrack(track));
        }
    }

    return song;
}

// The volume that the game gives its player for song `index`.
int SongVolume(std::span<const uint8_t> image, const LibultraProfile& profile, std::size_t index)
{
    if (index < profile.volume_list.size())
    {
        return profile.volume_list[index];
    }

    const SongVolumes& table = profile.volumes;
    if (table.stride == 0)
    {
        return profile.volume;
    }

    if (index >= table.count)
    {
        return table.other;
    }

    const std::size_t place = table.offset + index * table.stride;
    if (place + 2 > image.size())
    {
        return table.other;
    }

    return xsflib::LoadBe16(image.data() + place);
}

// The volume that Hudson's sound code gives the player for a song's volume byte v, read as signed: (v << 8) + 0xFF, or
// nothing for v <= 0. In Eleven Beat, the function at 0x80004268 reads the byte from the song's record, and the
// function at 0x80003358 makes the volume.
int HudsonVolume(uint8_t byte)
{
    const int value = static_cast<int8_t>(byte);
    return value > 0 ? (value << 8) + 0xFF : 0;
}

} // namespace

std::unique_ptr<LibultraGame> LibultraGame::Open(std::span<const uint8_t> image, const LibultraProfile* profile)
{
    std::unique_ptr<LibultraGame> game(new LibultraGame());
    game->image_ = image;
    if (profile != nullptr)
    {
        game->profile_ = *profile;
    }
    else
    {
        // Without a profile, the largest sequence file and the bank with the most instruments. The game's effect isn't
        // known, and the sequences' sends go out. Only a profile without an effect drops them.
        game->profile_.effect = std::nullopt;
        const n64::FoundFiles found = n64::FindFiles(image);
        std::size_t best_songs = 0;
        for (const std::size_t offset : found.sequence_banks)
        {
            const n64::SeqFile file = n64::SeqFile::Parse(image.subspan(offset));
            std::size_t songs = 0;
            for (std::size_t i = 0; i < file.Entries().size(); i++)
            {
                const std::span<const uint8_t> bytes = file.Sequence(i);
                songs += n64::CompressedMidi::Matches(bytes) || n64::StandardMidi::Matches(bytes) ? 1 : 0;
            }

            if (songs > best_songs)
            {
                best_songs = songs;
                game->profile_.sequences = offset;
            }
        }

        std::size_t best_instruments = 0;
        for (const std::size_t offset : found.bank_files)
        {
            const n64::BankFile file = n64::BankFile::Parse(image.subspan(offset));
            if (file.instruments.size() > best_instruments)
            {
                best_instruments = file.instruments.size();
                game->profile_.bank = offset;
            }
        }

        if (best_songs == 0 || best_instruments == 0)
        {
            return nullptr;
        }
    }

    if (game->profile_.midi_files != 0)
    {
        // A file ends where its only track ends.
        std::size_t place = game->profile_.sequences;
        for (std::size_t i = 0; i < game->profile_.midi_files; i++)
        {
            if (place + kMidiHeaderSize + kMidiTrackHeaderSize > image.size())
            {
                throw xsflib::FormatError("the game's MIDI files run past the image");
            }

            const std::size_t size =
                kMidiHeaderSize + kMidiTrackHeaderSize + xsflib::LoadBe32(image.data() + place + kMidiHeaderSize + 4);
            const std::span<const uint8_t> bytes = image.subspan(place, size);
            game->songs_.emplace_back(bytes.begin(), bytes.end());
            place += size;
        }
    }
    else if (game->profile_.rare_sequences)
    {
        const std::optional<std::size_t> place = n64::FindRareSequences(image);
        if (!place)
        {
            throw xsflib::FormatError("the game's table of sequences can't be found");
        }

        const n64::RareSequenceTable table = n64::RareSequenceTable::Parse(image.subspan(*place));
        for (std::size_t i = 0; i < table.entries.size(); i++)
        {
            game->songs_.push_back(table.Sequence(i));
        }
    }
    else
    {
        const std::span<const uint8_t> file = image.subspan(game->profile_.sequences);
        const bool hudson = game->profile_.hudson_sequences;
        const n64::SeqFile sequences =
            n64::SeqFile::Parse(file, hudson ? n64::kHudsonSeqFileRevision : n64::kSeqFileRevision);
        if (hudson)
        {
            game->records_ = n64::ReadHudsonRecords(file);
        }

        for (const n64::SeqFile::Entry& entry : sequences.Entries())
        {
            const std::span<const uint8_t> bytes = image.subspan(game->profile_.sequences + entry.offset, entry.length);
            game->songs_.emplace_back(bytes.begin(), bytes.end());
        }
    }

    game->banks_.push_back(game->ReadBanks(game->profile_.bank));
    for (const LibultraSongSet& set : game->profile_.other_sets)
    {
        game->banks_.push_back(game->ReadBanks(set.bank));
    }

    for (std::size_t i = 0; i < game->records_.size(); i++)
    {
        if (game->records_[i].bank >= game->banks_[0].file.banks.size())
        {
            throw xsflib::FormatError("song " + std::to_string(i) + " plays bank " +
                                      std::to_string(game->records_[i].bank) + ", which the bank file doesn't hold");
        }
    }

    return game;
}

LibultraGame::Banks LibultraGame::ReadBanks(std::size_t offset) const
{
    Banks banks;
    banks.file = n64::BankFile::Parse(image_.subspan(offset));
    if (profile_.program_bend_range != 0)
    {
        for (const n64::Bank& bank : banks.file.banks)
        {
            for (const int32_t object : bank.instruments)
            {
                if (object != n64::kNoObject)
                {
                    banks.file.instruments[static_cast<std::size_t>(object)].bend_range =
                        static_cast<int16_t>(profile_.program_bend_range);
                }
            }
        }
    }

    const std::optional<std::size_t> samples = n64::FindBankSamples(image_, banks.file);
    if (!samples)
    {
        throw xsflib::FormatError("the place of the bank's samples can't be found");
    }

    banks.samples = *samples;

    return banks;
}

std::size_t LibultraGame::SetSize(const LibultraSongSet& set) const
{
    const std::size_t first = std::min(set.first_song, songs_.size());
    return set.song_count != 0 ? std::min(set.song_count, songs_.size() - first) : songs_.size() - first;
}

std::size_t LibultraGame::SongCount() const
{
    return songs_.size();
}

std::span<const uint8_t> LibultraGame::SongFile(std::size_t number) const
{
    return songs_.at(number);
}

std::vector<ntdlib::sequence_export::Sequence> LibultraGame::Sequences() const
{
    // The songs that the profile lists, by their numbers in the file.
    const std::size_t first = std::min(profile_.first_song, songs_.size());
    const std::size_t end =
        profile_.song_count != 0 ? std::min(first + profile_.song_count, songs_.size()) : songs_.size();
    std::vector<ntdlib::sequence_export::Sequence> sequences;
    for (std::size_t i = first; i < end; i++)
    {
        sequences.push_back({i, SequenceLabel(i)});
    }

    std::size_t index = songs_.size();
    for (const LibultraRateChange& change : profile_.rate_changes)
    {
        sequences.push_back({index++, SequenceLabel(change.song, LibultraRateSettings(change))});
    }

    for (std::size_t set = 0; set < profile_.other_sets.size(); set++)
    {
        for (std::size_t i = 0; i < SetSize(profile_.other_sets[set]); i++)
        {
            sequences.push_back(
                {index++, SequenceLabel(profile_.other_sets[set].first_song + i, SetSettings(set + 1))});
        }
    }

    return sequences;
}

std::shared_ptr<const Wave> LibultraGame::WaveAt(Banks& banks, int32_t index) const
{
    const auto found = banks.waves.find(index);
    if (found != banks.waves.end())
    {
        return found->second;
    }

    const n64::BankFile& file = banks.file;
    const n64::BankWave& wave = file.waves[static_cast<std::size_t>(index)];
    const n64::BankLoop* loop =
        wave.loop == n64::kNoObject ? nullptr : &file.loops[static_cast<std::size_t>(wave.loop)];
    const n64::VadpcmBook* book =
        wave.type == n64::kWaveVadpcm ? &file.books.at(static_cast<std::size_t>(wave.book)).book : nullptr;
    // FindBankSamples checks only the first loops of a bank. A wave further on can run past the image.
    const std::size_t start = banks.samples + wave.base;
    if (start > image_.size() || wave.length > image_.size() - start)
    {
        throw xsflib::FormatError("a wave of the bank runs past the image");
    }

    std::shared_ptr<const Wave> out =
        DecodeWave(image_.subspan(start, wave.length), book, loop, HexName(wave.base), profile_.output_rate);
    banks.waves[index] = out;

    return out;
}

Conversion LibultraGame::Convert(std::size_t index, const PerformOptions& options)
{
    // A rate change's track comes after the songs, and the other sets' songs after the rate changes.
    const LibultraRateChange* change = nullptr;
    std::size_t song_index = index;
    std::size_t set_index = 0;
    std::string label_settings;
    if (index >= songs_.size() && index - songs_.size() < profile_.rate_changes.size())
    {
        change = &profile_.rate_changes[index - songs_.size()];
        song_index = change->song;
        label_settings = LibultraRateSettings(*change);
    }
    else if (index >= songs_.size())
    {
        std::size_t place = index - songs_.size() - profile_.rate_changes.size();
        for (const LibultraSongSet& set : profile_.other_sets)
        {
            set_index++;
            if (place < SetSize(set))
            {
                song_index = set.first_song + place;
                label_settings = SetSettings(set_index);
                break;
            }
            place -= SetSize(set);
        }
    }

    const LibultraSong song = ReadSong(songs_.at(song_index));
    Banks& banks = banks_.at(set_index);

    // Hudson's sequence bank gives each song its bank and its volume.
    const n64::HudsonSongRecord* record = records_.empty() ? nullptr : &records_.at(song_index);
    const n64::Bank& bank = banks.file.banks.at(record != nullptr ? record->bank : 0);

    LibultraSettings settings;
    settings.player = profile_.player;
    settings.line_ramps = profile_.line_ramps;
    settings.volume = record != nullptr ? HudsonVolume(record->volume) : SongVolume(image_, profile_, song_index);
    settings.oscillators = profile_.oscillators;
    settings.effect = profile_.effect;
    if (change != nullptr)
    {
        settings.rate = change->rate;
    }

    LibultraPerformance played = PerformLibultra(song, banks.file, bank, settings, options);
    for (const int32_t instrument : played.instruments)
    {
        char name[32];
        std::snprintf(name, sizeof(name), "i%d", instrument);

        Instrument preset;
        preset.name = name;
        std::vector<int32_t> waves;
        preset.zones = LibultraZones(banks.file, instrument, settings, waves);
        for (std::size_t i = 0; i < preset.zones.size(); i++)
        {
            preset.zones[i].wave = WaveAt(banks, waves[i]);
        }

        played.performance.presets.push_back(std::move(preset));
    }

    return BuildFiles(std::move(played.performance), SequenceLabel(song_index, label_settings), options.program,
                      options.resampler_filter);
}

} // namespace ultralib::sequence_export::n64
