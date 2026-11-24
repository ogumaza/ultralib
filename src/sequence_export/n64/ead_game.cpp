// SPDX-License-Identifier: MIT

#include <algorithm>
#include <cstdio>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "ead_shared.h"
#include "ultralib/n64/rom_data.h"
#include "ultralib/n64/seq_file.h"
#include "ultralib/n64/vadpcm.h"
#include "ultralib/sequence_export/n64/ead.h"
#include "ultralib/sequence_export/n64/wave.h"

namespace ultralib::sequence_export::n64
{

namespace
{

namespace n64 = ultralib::n64;
using ead::IsLater;

} // namespace

std::unique_ptr<EadGame> EadGame::Open(std::span<const uint8_t> image, const EadProfile& profile)
{
    if (IsLater(profile.engine))
    {
        std::optional<n64::EadLaterFiles> files = n64::EadLaterFiles::Read(image);
        if (!files)
        {
            return nullptr;
        }

        std::unique_ptr<EadGame> game(new EadGame());
        game->image_ = image;
        game->profile_ = profile;
        game->later_files_ = std::move(files);
        game->bank_sets_ = game->later_files_->bank_sets;
        for (const std::vector<uint8_t>& sequence : game->later_files_->sequences)
        {
            game->sequences_.emplace_back(sequence);
        }

        return game;
    }

    const std::optional<n64::EarlySoundData> data = n64::FindEarlySoundData(image);
    if (!data)
    {
        return nullptr;
    }

    std::unique_ptr<EadGame> game(new EadGame());
    game->image_ = image;
    game->profile_ = profile;
    const auto entries = [&](std::size_t offset, uint16_t revision, std::vector<std::span<const uint8_t>>& out)
    {
        const n64::SeqFile file = n64::SeqFile::Parse(image.subspan(offset), revision);
        for (const n64::SeqFile::Entry& entry : file.Entries())
        {
            out.push_back(image.subspan(offset + entry.offset, entry.length));
        }
    };

    entries(data->sequences, n64::kEarlySequenceFileRevision, game->sequences_);
    entries(data->banks, n64::kEarlyBankFileRevision, game->bank_files_);
    entries(data->samples, n64::kEarlySampleFileRevision, game->sample_files_);
    if (data->bank_sets)
    {
        game->bank_sets_ = n64::ParseEarlyBankSets(image.subspan(*data->bank_sets), game->sequences_.size());
    }

    return game;
}

std::vector<ntdlib::sequence_export::Sequence> EadGame::Sequences() const
{
    // A track that the profile lists comes after the sequences in the numbering.
    std::vector<ntdlib::sequence_export::Sequence> list;
    for (std::size_t index = 0; index < sequences_.size(); index++)
    {
        if (std::ranges::find(profile_.effect_sequences, index) != profile_.effect_sequences.end() ||
            sequences_[index].empty())
        {
            continue;
        }

        // A later engine's table can name one sequence twice. Without a track of its own, the second adds nothing.
        const bool has_track =
            std::ranges::any_of(profile_.tracks, [&](const EadTrack& track) { return track.sequence == index; });
        const auto same_as = [&](std::size_t earlier)
        {
            return std::ranges::equal(sequences_[earlier], sequences_[index]) &&
                   bank_sets_[earlier] == bank_sets_[index];
        };

        if (!has_track && later_files_ && std::ranges::any_of(std::views::iota(std::size_t{0}, index), same_as))
        {
            continue;
        }

        if (!has_track)
        {
            list.push_back({index, SequenceLabel(index)});
        }

        for (std::size_t i = 0; i < profile_.tracks.size(); i++)
        {
            if (profile_.tracks[i].sequence == index)
            {
                list.push_back({sequences_.size() + i, EadTrackLabel(profile_, profile_.tracks[i])});
            }
        }
    }

    return list;
}

const n64::EadBank& EadGame::BankAt(int number)
{
    const auto found = banks_.find(number);
    if (found != banks_.end())
    {
        return found->second;
    }

    if (later_files_)
    {
        return banks_.emplace(number, later_files_->banks.at(static_cast<std::size_t>(number))).first->second;
    }

    return banks_.emplace(number, n64::EadBank::ParseEarly(bank_files_.at(static_cast<std::size_t>(number))))
        .first->second;
}

std::shared_ptr<const Wave> EadGame::WaveAt(int bank_number, int32_t sample, int gain)
{
    if (gain != 0)
    {
        const auto key = std::make_tuple(bank_number, sample, gain);
        const auto found = gained_waves_.find(key);
        if (found != gained_waves_.end())
        {
            return found->second;
        }

        // The engine multiplies a note's samples by the gain and limits them to 16 bits.
        auto out = std::make_shared<Wave>(*WaveAt(bank_number, sample, 0));
        char suffix[8];
        std::snprintf(suffix, sizeof(suffix), "g%02X", static_cast<unsigned>(gain));
        out->name += suffix;
        for (std::vector<int16_t>& channel : out->pcm.channels)
        {
            for (int16_t& value : channel)
            {
                value = static_cast<int16_t>(std::clamp((value * gain) >> 4, -32768, 32767));
            }
        }

        gained_waves_[key] = out;

        return out;
    }

    const auto key = std::make_tuple(bank_number, sample);
    const auto found = waves_.find(key);
    if (found != waves_.end())
    {
        return found->second;
    }

    const n64::EadBank& bank = BankAt(bank_number);
    const n64::EadSample& object = bank.samples.at(static_cast<std::size_t>(sample));
    std::span<const uint8_t> file;
    if (later_files_)
    {
        const std::array<uint32_t, 2>& places = later_files_->sample_places.at(static_cast<std::size_t>(bank_number));
        file = image_.subspan(places[object.medium < 2 ? object.medium : 0]);
    }
    else
    {
        file = sample_files_.at(static_cast<std::size_t>(bank_number));
    }

    const std::span<const uint8_t> bytes = file.subspan(object.address, object.size);
    const bool small = object.codec == n64::kEadCodecSmallAdpcm;
    auto out = std::make_shared<Wave>();
    char name[32];
    std::snprintf(name, sizeof(name), "b%02Xs%06X", static_cast<unsigned>(bank_number), object.address);
    out->name = name;
    out->rate = static_cast<uint32_t>(profile_.wave_rate);

    if (object.codec != n64::kEadCodecAdpcm && !small)
    {
        // The engines decode nothing else from an image, except signed bytes. Their other kinds play as silence.
        std::vector<int16_t> pcm;
        if (object.codec == n64::kEadCodecS8)
        {
            pcm.reserve(bytes.size());
            for (const uint8_t byte : bytes)
            {
                pcm.push_back(static_cast<int16_t>(static_cast<int8_t>(byte) * 256));
            }
        }

        if (pcm.empty())
        {
            pcm.assign(n64::kVadpcmFrameSamples, 0);
        }

        out->pcm.channels.push_back(std::move(pcm));
        waves_[key] = out;

        return out;
    }

    const n64::VadpcmBook& book = bank.books.at(static_cast<std::size_t>(object.book)).book;
    const n64::BankLoop* loop =
        object.loop != n64::kNoObject ? &bank.loops.at(static_cast<std::size_t>(object.loop)) : nullptr;
    out->pcm = DecodeWaveSamples(bytes, &book, loop, small ? VadpcmFrames::kSmall : VadpcmFrames::kStandard);

    // A loop with a count of 0 ends the wave at the loop's end.
    std::vector<int16_t>& samples = out->pcm.channels[0];
    if (loop != nullptr && loop->count == 0 && loop->end > 0 && loop->end < samples.size())
    {
        samples.resize(loop->end);
    }

    waves_[key] = out;

    return out;
}

Conversion EadGame::Convert(std::size_t index, const PerformOptions& options)
{
    if (index >= sequences_.size())
    {
        const EadTrack& track = profile_.tracks[index - sequences_.size()];

        return ConvertTrack(track, EadTrackLabel(profile_, track), options);
    }

    return ConvertTrack({.sequence = index, .variation = profile_.variation}, SequenceLabel(index), options);
}

Conversion EadGame::ConvertTrack(const EadTrack& track, const std::string& name, const PerformOptions& options)
{
    const std::size_t index = track.sequence;
    EadPerformSettings settings;
    settings.engine = profile_.engine;
    settings.levels = profile_.levels;
    settings.envelope_skip = profile_.envelope_skip;
    settings.channel_layers = profile_.channel_layers;
    settings.inverse_vibrato = profile_.inverse_vibrato;
    settings.sequence = sequences_.at(index);
    if (index < bank_sets_.size())
    {
        settings.bank_set = bank_sets_[index];
    }

    for (const uint8_t number : settings.bank_set)
    {
        BankAt(number);
    }

    settings.banks = &banks_;
    settings.clock.updates_per_frame = profile_.updates_per_frame;
    settings.clock.output_rate = profile_.output_rate;
    settings.clock.wave_rate = profile_.wave_rate;
    settings.player_scale = profile_.player_volume;
    settings.variation = track.variation;
    settings.silent_channels = track.silent_channels;
    settings.tempo_change = track.tempo_change;
    std::ranges::copy(track.ports, settings.ports.begin());
    settings.call = track.call;
    settings.sequences = sequences_;
    settings.bank_sets = bank_sets_;
    if (later_files_)
    {
        // A script may start or load any other sequence.
        for (std::size_t bank = 0; bank < later_files_->banks.size(); bank++)
        {
            BankAt(static_cast<int>(bank));
        }
    }

    // The game's code scales the player by a level for the sequence.
    if (profile_.player_level_count != 0 && track.sequence >= profile_.first_player_level &&
        track.sequence - profile_.first_player_level < profile_.player_level_count)
    {
        constexpr int kHighestLevel = 127;
        int level = image_[profile_.player_levels + track.sequence - profile_.first_player_level];
        if (level > kHighestLevel)
        {
            level = image_[profile_.player_levels_indirect + static_cast<std::size_t>(level - kHighestLevel - 1)];
        }

        settings.player_scale = profile_.player_volume * level / kHighestLevel;
    }

    // A table in the game's code gives the song's volume, or the script does.
    if (index < profile_.song_volumes_count)
    {
        settings.song_volume = image_[profile_.song_volumes + index] / 127.0;
    }

    // The game's code can give a sequence another volume.
    if (track.song_volume >= 0)
    {
        settings.song_volume = track.song_volume / 127.0;
    }

    EadPerformance played = PerformEad(settings, options);
    for (const EadPresetKey& key : played.presets)
    {
        char preset_name[32];
        std::snprintf(preset_name, sizeof(preset_name), "b%d p%d", key.bank, key.program);

        Instrument preset;
        preset.name = preset_name;
        std::vector<int32_t> samples;
        preset.zones = EadZones(key, BankAt(key.bank), settings, samples);
        for (std::size_t i = 0; i < preset.zones.size(); i++)
        {
            preset.zones[i].wave = WaveAt(key.bank, samples[i], key.sample_gain);
        }

        played.performance.presets.push_back(std::move(preset));
    }

    return BuildFiles(std::move(played.performance), name, options.program, options.resampler_filter);
}

} // namespace ultralib::sequence_export::n64
