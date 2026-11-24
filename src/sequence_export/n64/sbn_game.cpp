// SPDX-License-Identifier: MIT

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <memory>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/bytes.h"

#include "rsp_audio.h"
#include "sbn_shared.h"
#include "ultralib/n64/bank_file.h"
#include "ultralib/n64/vadpcm.h"
#include "ultralib/sequence_export/n64/envelope.h"
#include "ultralib/sequence_export/n64/sbn.h"
#include "ultralib/sequence_export/n64/wave.h"

namespace ultralib::sequence_export::n64
{

namespace
{

namespace n64 = ultralib::n64;
using sbn::DrumOf;
using sbn::kCentrePan;
using sbn::kCentsPerKey;
using sbn::kEnvelopeScale;
using sbn::kFirstEnvelopeCommand;
using sbn::kFullLevel;
using sbn::kHighestPan;
using sbn::kHighestRatio;
using sbn::kLevelMask;
using sbn::kNominalRate;
using sbn::kRelativeLevel;
using sbn::kVelocityScale;

// The size of a table of track levels in the image.
constexpr std::size_t kTrackLevelsBytes = 32;

// The level that the game's code at 0x80056044 gives a mix's tracks at the near state.
constexpr int kNearLevel = 0x57;

// The compositions that a song can hold.
constexpr int kCompositions = 4;

// What the game's code can change in a composition: the values that its branches read, and its linked tracks.
std::pair<int, bool> ChoicesOf(const n64::BgmFile& file, uint32_t composition)
{
    int mixes = 1;
    bool links = false;
    for (const n64::BgmCompositionCommand& command : file.Composition(composition))
    {
        if (command.Op() != n64::BgmCompositionOp::kPlayPhrase)
        {
            continue;
        }

        for (const n64::BgmTrackInfo& info : file.Phrase(command.PhraseOffset(composition)))
        {
            if (info.offset == 0 || info.disabled)
            {
                continue;
            }

            links = links || info.linked_track != 0;
            n64::BgmCursor cursor;
            cursor.position = info.offset;
            n64::BgmCommand read = n64::ReadBgmCommand(file.Bytes(), cursor);
            while (read.op != n64::BgmOp::kEnd)
            {
                if (read.op == n64::BgmOp::kBranch)
                {
                    mixes = std::max<int>(mixes, read.args[2]);
                    const n64::BgmBranchTarget target = n64::ReadBgmBranch(file.Bytes(), read, 0);
                    cursor.branch_end = cursor.position;
                    cursor.position = target.position;
                }

                read = n64::ReadBgmCommand(file.Bytes(), cursor);
            }
        }
    }

    return {mixes, links};
}

// A voice's volume at the top of every factor but the envelope, on a scale of 32768. The engine multiplies the master
// and instrument volumes and the proximity's volume in 10 bits each, then the track's volume and the velocity.
constexpr int kFullFactors = (((kFullLevel << 3) * (kFullLevel << 3) * 1023) >> 20) * kFullLevel * 128 >> 9;
constexpr double kFullVoice = kFullFactors / 32768.0;

double PanOf(int pan)
{
    return pan >= kCentrePan ? static_cast<double>(pan - kCentrePan) / (kHighestPan - kCentrePan)
                             : static_cast<double>(pan - kCentrePan) / kCentrePan;
}

double TimeOf(std::span<const int32_t> times, uint8_t index)
{
    return index < times.size() ? times[index] : 0.0;
}

// An envelope's press list as a shape in microseconds. No list of the game's holds a command. A command ends a list.
EnvelopeShape PressShape(std::span<const uint8_t> press, std::span<const int32_t> times)
{
    EnvelopeShape shape;
    shape.points.push_back({0.0, 0.0});
    double time = 0.0;
    for (std::size_t at = 0; at + 1 < press.size() && press[at] < kFirstEnvelopeCommand; at += 2)
    {
        const double level = (press[at + 1] & kLevelMask) / kEnvelopeScale;
        time += TimeOf(times, press[at]);
        shape.points.push_back({time, level * level});
    }

    return shape;
}

// The energy of a release list from a level of 1, in microseconds at that level. `from` is the amplitude that the
// release starts from, for a list whose levels aren't relative to it.
double ReleaseEnergy(std::span<const uint8_t> release, std::span<const int32_t> times, double from)
{
    bool relative = release.size() > 1 && (release[1] & kRelativeLevel) != 0;
    double before = 1.0;
    double energy = 0.0;
    for (std::size_t at = 0; at + 1 < release.size() && release[at] < kFirstEnvelopeCommand; at += 2)
    {
        const double level = (release[at + 1] & kLevelMask) / kEnvelopeScale;
        const double next = relative ? level * level : level * level / std::max(from, 1e-6);
        energy += TimeOf(times, release[at]) * (before * before + before * next + next * next) / 3.0;
        before = next;
    }

    return energy;
}

// A zone's envelope and level.
void FitSbnEnvelope(Zone& zone, const SbnSound& sound, std::span<const uint8_t> custom_press,
                    std::span<const int32_t> times, uint32_t output_rate)
{
    // The table's times count frames at the nominal rate.
    const double updates_per_second = 1e6 * output_rate / kNominalRate;
    const EnvelopeShape shape = PressShape(custom_press.empty() ? sound.press : custom_press, times);
    const double level = FitEnvelope(zone, shape, updates_per_second, 0.0);
    const double held = shape.points.back().level > 0.0 ? shape.points.back().level : level;

    // SoundFont's release falls 100 dB in a straight line of dB. A fall that takes T seconds holds T / (10 ln 10) of
    // energy at a level of 1. The zone's release holds the energy of the game's list.
    constexpr double kEnergyTo100Db = 10.0 * std::numbers::ln10;
    zone.release_ms = kEnergyTo100Db * ReleaseEnergy(sound.release, times, held) * 1000.0 / updates_per_second;
    zone.gain = level * kFullVoice * kFullVoice;
}

} // namespace

std::vector<int> SbnVariations(const n64::BgmFile& file)
{
    std::vector<int> variations;
    std::vector<std::vector<uint32_t>> seen;
    for (int variation = 0; variation < kCompositions; variation++)
    {
        const uint32_t composition = file.CompositionOffset(variation);
        if (composition == 0)
        {
            continue;
        }

        // A phrase's place in the file, and each other command as it stands. Two compositions can share a phrase.
        std::vector<uint32_t> list;
        bool plays = false;
        for (const n64::BgmCompositionCommand& command : file.Composition(composition))
        {
            const bool phrase = command.Op() == n64::BgmCompositionOp::kPlayPhrase;
            plays = plays || phrase;
            list.push_back(phrase ? command.PhraseOffset(composition) : command.raw);
        }

        if (plays && std::ranges::find(seen, list) == seen.end())
        {
            seen.push_back(std::move(list));
            variations.push_back(variation);
        }
    }

    return variations;
}

double SbnReleaseSeconds(std::span<const uint8_t> release, std::span<const int32_t> times, uint32_t output_rate)
{
    double micros = 0.0;
    for (std::size_t at = 0; at + 1 < release.size() && release[at] < kFirstEnvelopeCommand; at += 2)
    {
        micros += TimeOf(times, release[at]);
    }

    return micros / 1e6 * kNominalRate / output_rate;
}

std::vector<Zone> SbnInstrumentZones(const SbnSound& sound, const SbnPresetKey& key, std::span<const int32_t> times,
                                     uint32_t output_rate)
{
    Zone zone;
    zone.exclusive_class = key.cuts ? 1 : 0;

    // A key's pitch stands 100 * key + base cents above the output's rate.
    const double base =
        1200.0 * std::log2(static_cast<double>(sound.sample_rate) / kNominalRate) - sound.key_base + key.fine_cents;
    zone.root_key = static_cast<int>(std::clamp(std::lround(-base / kCentsPerKey), 0L, 127L));
    zone.tune_cents = base + kCentsPerKey * zone.root_key;
    FitSbnEnvelope(zone, sound, key.press, times, output_rate);
    if (key.tremolo_depth != 0)
    {
        constexpr double kHzSteps = 16.0;
        zone.vibrato = Lfo{.hz = key.tremolo_hz_sixteenths / kHzSteps,
                           .delay_ms = static_cast<double>(key.tremolo_delay_ms),
                           .depth = static_cast<double>(key.tremolo_depth)};
    }

    const double limit_cents = 1200.0 * std::log2(kHighestRatio);
    std::vector<Zone> zones;
    AddUpToResamplerLimit(zones, zone, static_cast<int>(std::ceil((limit_cents - base) / kCentsPerKey)), limit_cents);

    return zones;
}

Zone SbnDrumZone(const SbnSound& sound, const n64::BgmDrum& drum, int key, int tune_cents,
                 std::span<const int32_t> times, uint32_t output_rate)
{
    Zone zone;
    zone.low_key = key;
    zone.high_key = key;
    zone.root_key = key;
    zone.follows_key = false;
    zone.pan = PanOf(drum.pan);
    const double cents = 1200.0 * std::log2(static_cast<double>(sound.sample_rate) / kNominalRate) + drum.key_base +
                         tune_cents - sound.key_base;
    zone.tune_cents = std::min(cents, 1200.0 * std::log2(kHighestRatio));
    FitSbnEnvelope(zone, sound, {}, times, output_rate);

    // A drum's volume counts out of 128, in place of the shift of the track's volume. The send's share comes off here,
    // because the reverb is the drum's.
    const double volume = drum.volume / kVelocityScale;
    zone.gain *= volume * volume * std::max(DryShare(drum.reverb), 0.0);

    return zone;
}

std::unique_ptr<SbnGame> SbnGame::Open(std::span<const uint8_t> image, const SbnProfile& profile)
{
    const std::size_t at = n64::FindSbnArchive(image);
    if (at >= image.size())
    {
        throw xsflib::FormatError("the image holds no SBN archive");
    }

    std::unique_ptr<SbnGame> game(new SbnGame(image, profile, n64::SbnArchive::Parse(image.subspan(at))));

    return game;
}

n64::SbnTables ReadSbnTables(std::span<const uint8_t> image, const SbnProfile& profile)
{
    // The table of the factors of a tuning: 128 and 32 above unity, then 128 and 128 below.
    constexpr std::size_t kTuneFactors = 416;

    n64::SbnTables tables;
    std::copy_n(image.begin() + profile.ticks_per_beat, tables.ticks_per_beat.size(), tables.ticks_per_beat.begin());
    std::copy_n(image.begin() + profile.default_envelope, tables.default_envelope.size(),
                tables.default_envelope.begin());
    std::copy_n(image.begin() + profile.custom_times, tables.custom_envelope_times.size(),
                tables.custom_envelope_times.begin());
    for (std::size_t i = 0; i < profile.envelope_time_count; i++)
    {
        tables.envelope_times.push_back(
            static_cast<int32_t>(xsflib::LoadBe32(image.data() + profile.envelope_times + i * 4)));
    }

    for (std::size_t i = 0; i < kTuneFactors; i++)
    {
        tables.tune_scaling.push_back(
            std::bit_cast<float>(xsflib::LoadBe32(image.data() + profile.tune_scaling + i * 4)));
    }

    return tables;
}

SbnGame::SbnGame(std::span<const uint8_t> image, const SbnProfile& profile, n64::SbnArchive archive)
    : image_(image), profile_(&profile), archive_(std::move(archive)), tables_(ReadSbnTables(image, profile))
{
    // The first three of the other files: the sound effects, the drums and the instruments.
    const std::vector<uint16_t>& others = archive_.OtherFiles();
    per_ = n64::ParsePerFile(archive_.File(others.at(1)));
    prg_ = n64::ParsePrgFile(archive_.File(others.at(2)));
    for (const n64::SbnArchive::InitBank& bank : archive_.InitBanks())
    {
        init_files_.at(bank.bank_set).at(bank.bank) = bank.file;
    }

    // A song that repeats an earlier song's files adds nothing.
    std::set<std::tuple<uint16_t, uint16_t, uint16_t, uint16_t>> seen;
    const std::vector<n64::SbnArchive::Song>& songs = archive_.Songs();
    for (std::size_t s = 0; s < songs.size(); s++)
    {
        const n64::SbnArchive::Song& song = songs[s];
        if (song.bgm >= archive_.Entries().size() ||
            !seen.insert({song.bgm, song.banks[0], song.banks[1], song.banks[2]}).second)
        {
            continue;
        }

        const std::span<const uint8_t> bytes = archive_.File(song.bgm);
        if (!n64::BgmFile::Matches(bytes))
        {
            continue;
        }

        const n64::BgmFile file = n64::BgmFile::Parse(bytes);
        const auto name_of = [&](int variation, const std::string& change)
        {
            std::string settings = variation != 0 ? "variation " + std::to_string(variation) : "";
            if (!change.empty())
            {
                settings += (settings.empty() ? "" : ", ") + change;
            }

            return SequenceLabel(s, settings);
        };

        for (const int variation : SbnVariations(file))
        {
            const auto [mixes, links] = ChoicesOf(file, file.CompositionOffset(variation));
            for (int mix = 0; mix < mixes; mix++)
            {
                for (int linked = 0; linked <= (links ? 1 : 0); linked++)
                {
                    const auto change = std::ranges::find_if(
                        profile_->changes, [&](const SbnChange& c)
                        { return c.song == s && c.levels == 0 && c.mix == mix && c.linked == (linked != 0); });
                    const bool known = change != profile_->changes.end();

                    // The game's triggers pick a mix only in the song's first version, and only the mixes in its
                    // profile.
                    if (mix != 0 && (variation != 0 || !known))
                    {
                        continue;
                    }

                    std::string detail;
                    if (known)
                    {
                        detail = SbnChangeSettings(*change);
                    }
                    else if (linked != 0)
                    {
                        detail = "linked";
                    }

                    tracks_.push_back({.song = s,
                                       .variation = variation,
                                       .mix = mix,
                                       .linked = linked != 0,
                                       .near = known && change->near,
                                       .name = name_of(variation, detail)});
                }
            }

            if (variation != 0)
            {
                continue;
            }

            for (const SbnChange& change : profile_->changes)
            {
                if (change.song == s && change.levels != 0)
                {
                    tracks_.push_back(
                        {.song = s, .levels = change.levels, .name = name_of(0, SbnChangeSettings(change))});
                }
            }
        }
    }
}

std::vector<ntdlib::sequence_export::Sequence> SbnGame::Sequences() const
{
    std::vector<ntdlib::sequence_export::Sequence> sequences;
    for (std::size_t i = 0; i < tracks_.size(); i++)
    {
        sequences.push_back({i, tracks_[i].name});
    }

    return sequences;
}

const n64::BkFile& SbnGame::Bank(uint16_t file)
{
    const auto found = banks_.find(file);
    if (found != banks_.end())
    {
        return found->second;
    }

    return banks_.emplace(file, n64::BkFile::Parse(archive_.File(file))).first->second;
}

std::optional<SbnGame::Found> SbnGame::Find(uint16_t bank_patch, const n64::SbnArchive::Song& song)
{
    // The number of a set of banks in a song's note, and the game's number of the same set: 0 and 7 are the song's
    // banks, 2 has no banks.
    constexpr std::array<int, 8> kSetOfNumber = {1, 2, 0, 3, 4, 5, 6, 1};
    constexpr int kSongSet = 1;
    const int bank = bank_patch >> 8;
    const std::size_t group = (bank_patch & 0xFF) >> 4;
    const std::size_t slot = bank_patch & 0xF;
    const int set = kSetOfNumber.at(static_cast<std::size_t>((bank & 0x70) >> 4));
    uint16_t file = init_files_.at(static_cast<std::size_t>(set)).at(group);
    if (set == kSongSet && group < song.banks.size() && song.banks[group] != 0)
    {
        file = song.banks[group];
    }

    if (file == 0 || file >= archive_.Entries().size())
    {
        return std::nullopt;
    }

    const n64::BkFile& bk = Bank(file);
    const int32_t index = bk.slots.at(slot);
    if (index < 0 || bk.instruments.at(static_cast<std::size_t>(index)).wave == 0)
    {
        return std::nullopt;
    }

    return Found{&bk, &bk.instruments[static_cast<std::size_t>(index)], file};
}

SbnSound SbnGame::SoundOf(const Found& found, uint16_t bank_patch) const
{
    const n64::BkInstrument& instrument = *found.instrument;
    SbnSound sound;
    sound.key_base = instrument.key_base;
    sound.sample_rate = instrument.sample_rate;

    const std::span<const uint8_t> bytes = found.bank->Bytes();
    const std::size_t choice = (bank_patch >> 8) & 3;
    if (instrument.envelopes != 0 && choice < bytes[instrument.envelopes])
    {
        const std::size_t entry = instrument.envelopes + 4 + choice * 4;
        sound.press = bytes.subspan(instrument.envelopes + xsflib::LoadBe16(bytes.data() + entry));
        sound.release = bytes.subspan(instrument.envelopes + xsflib::LoadBe16(bytes.data() + entry + 2));
    }
    else
    {
        // The game's default envelope: a list of press and a list of release, of one step each.
        constexpr std::size_t kDefaultListBytes = 4;
        const std::span<const uint8_t> lists = tables_.default_envelope;
        sound.press = lists.first(kDefaultListBytes);
        sound.release = lists.subspan(kDefaultListBytes, kDefaultListBytes);
    }

    return sound;
}

std::shared_ptr<const Wave> SbnGame::WaveOf(const Found& found)
{
    const n64::BkInstrument& instrument = *found.instrument;
    const auto key = std::make_pair(found.file, instrument.offset);
    const auto cached = waves_.find(key);
    if (cached != waves_.end())
    {
        return cached->second;
    }

    constexpr uint8_t kVadpcm = 0;
    n64::VadpcmBook book;
    n64::BankLoop loop;
    const bool vadpcm = instrument.type == kVadpcm;
    if (vadpcm)
    {
        book = found.bank->Book(instrument);
    }

    const bool loops = instrument.loop_end != 0 && instrument.loop_count != 0;
    if (loops)
    {
        loop.start = instrument.loop_start;
        loop.end = instrument.loop_end;
        loop.count = instrument.loop_count;
        if (vadpcm && instrument.loop_state != 0)
        {
            loop.has_state = true;
            loop.state = found.bank->LoopState(instrument);
        }
    }

    std::span<const uint8_t> bytes = found.bank->Wave(instrument);
    if (vadpcm)
    {
        bytes = bytes.first(bytes.size() / n64::kVadpcmFrameSize * n64::kVadpcmFrameSize);
    }

    char name[32];
    std::snprintf(name, sizeof(name), "f%u_%04X", found.file, instrument.offset);
    std::shared_ptr<const Wave> wave =
        DecodeWave(bytes, vadpcm ? &book : nullptr, loops ? &loop : nullptr, name, profile_->output_rate);
    waves_[key] = wave;

    return wave;
}

Conversion SbnGame::Convert(std::size_t index, const PerformOptions& options)
{
    const Track& track = tracks_.at(index);
    const n64::SbnArchive::Song& song = archive_.Songs().at(track.song);
    const n64::BgmFile file = n64::BgmFile::Parse(archive_.File(song.bgm));

    SbnSettings settings;
    settings.output_rate = profile_->output_rate;
    settings.ticks_per_beat = tables_.ticks_per_beat[file.timing_preset & 7U]; // the game reads 3 bits
    settings.variation = track.variation;
    settings.mix = track.mix;
    settings.linked = track.linked;
    settings.proximity = track.near ? kNearLevel : kFullLevel;
    if (track.levels != 0)
    {
        settings.track_levels = image_.subspan(track.levels, kTrackLevelsBytes);
    }

    settings.custom_times = tables_.custom_envelope_times;

    const SbnFacts facts = [&](uint16_t bank_patch)
    {
        SbnInstrumentFacts out;
        const std::optional<Found> found = Find(bank_patch, song);
        if (!found)
        {
            return out;
        }

        const n64::BkInstrument& instrument = *found->instrument;
        const SbnSound sound = SoundOf(*found, bank_patch);
        out.exists = true;
        out.key_base = instrument.key_base;
        out.sample_rate = instrument.sample_rate;
        out.samples = static_cast<double>(instrument.wave_size / n64::kVadpcmFrameSize * n64::kVadpcmFrameSamples);
        out.loops = instrument.loop_end != 0 && instrument.loop_count != 0;
        out.release_seconds = SbnReleaseSeconds(sound.release, tables_.envelope_times, profile_->output_rate);

        return out;
    };

    SbnPerformance played = PerformSbn(file, per_, prg_, facts, settings, options);
    for (const SbnPresetKey& key : played.presets)
    {
        Instrument preset;
        if (key.drums)
        {
            preset.name = "drums";
            for (const int drum_key : played.drum_keys)
            {
                const n64::BgmDrum* drum = DrumOf(file, per_, drum_key);
                if (drum == nullptr)
                {
                    continue;
                }

                const std::optional<Found> found = Find(drum->bank_patch, song);
                if (!found)
                {
                    continue;
                }

                Zone zone = SbnDrumZone(SoundOf(*found, drum->bank_patch), *drum, drum_key, key.fine_cents,
                                        tables_.envelope_times, profile_->output_rate);
                zone.exclusive_class = key.cuts ? 1 : 0;
                zone.wave = WaveOf(*found);
                preset.zones.push_back(std::move(zone));
            }
        }
        else
        {
            char name[32];
            std::snprintf(name, sizeof(name), "%04X", key.bank_patch);
            preset.name = name;
            const std::optional<Found> found = Find(key.bank_patch, song);
            if (found)
            {
                for (Zone& zone : SbnInstrumentZones(SoundOf(*found, key.bank_patch), key, tables_.envelope_times,
                                                     profile_->output_rate))
                {
                    zone.wave = WaveOf(*found);
                    preset.zones.push_back(std::move(zone));
                }
            }
        }

        played.performance.presets.push_back(std::move(preset));
    }

    return BuildFiles(std::move(played.performance), track.name, options.program, options.resampler_filter);
}

} // namespace ultralib::sequence_export::n64
