// SPDX-License-Identifier: MIT

#include "ultralib/n64/sbn_player.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

#include "xsflib/bytes.h"

#include "sbn_player_constants.h"
#include "ultralib/n64/audio_list.h"
#include "ultralib/n64/audio_memory.h"
#include "ultralib/n64/sbn.h"

namespace ultralib::n64
{

namespace
{

using sbn_player_constants::kChunk;
using sbn_player_constants::kChunkBytes;
using sbn_player_constants::kDelayLeft;
using sbn_player_constants::kDelayNone;
using sbn_player_constants::kDelayRight;
using sbn_player_constants::kEndCommand;
using sbn_player_constants::kFrameMicroseconds;
using sbn_player_constants::kFxLength;
using sbn_player_constants::kSyncAll;
using sbn_player_constants::kSyncPanMix;
using sbn_player_constants::kSyncParams;
using sbn_player_constants::kSyncPitch;
using sbn_player_constants::kTempoScale;
using sbn_player_constants::kUpdateScale;

constexpr int kDefaultBpm = 156;
constexpr int32_t kDefaultStep = 156250;
constexpr uint32_t kDefaultTicksPerBeat = 48;
constexpr uint32_t kFramesPerMinute = kUpdateScale * (60 * 32000) / kChunk;

constexpr uint8_t kStateIdle = 0;
constexpr uint8_t kStateActive = 1;
constexpr uint8_t kStateFetch = 2;
constexpr uint8_t kStateInit = 3;
constexpr uint8_t kStateStop = 4;

} // namespace

SbnPlayer::SbnPlayer(const SbnPlayerConfig& config, const SbnArchive& archive, AudioMemory& memory)
    : config_(config), archive_(&archive)
{
    Wave fallback;
    fallback.key_base = 4800;
    fallback.pitch_ratio = 0.5F;
    waves_.push_back(fallback);
    default_wave_ = 0;
    for (auto& set : bank_sets_)
    {
        for (auto& bank : set)
        {
            bank.fill(default_wave_);
        }
    }

    for (Voice& voice : voices_)
    {
        voice.decoder_state = memory.Allocate(32);
        voice.decoder_loop_state = memory.Allocate(32);
        voice.resampler_state = memory.Allocate(32);
        voice.mixer_state = memory.Allocate(80);
    }

    dry_accumulator_ = memory.Allocate(kChunkBytes * 2);
    wet_accumulator_ = memory.Allocate(kChunkBytes * 2);
    delay_main_ = memory.Allocate(4 * kChunk * 2);
    delay_aux_ = memory.Allocate(4 * kChunk * 2);
    for (Bus& bus : buses_)
    {
        for (Effect& effect : bus.effects)
        {
            effect.base = memory.Allocate(kFxLength * 2);
            for (Delay& delay : effect.delays)
            {
                delay.resampler_state = memory.Allocate(32);
                delay.low_pass.coefficients_address = memory.Allocate(32);
                delay.low_pass.state = memory.Allocate(32);
            }
        }
    }

    build_memory_ = &memory;
    const auto& others = archive.OtherFiles();
    if (others.size() > 1)
    {
        const std::vector<BgmDrum> drums = ParsePerFile(archive.File(others[1]));
        for (std::size_t i = 0; i < global_drums_.size() && i < drums.size(); i++)
        {
            global_drums_[i] = drums[i];
        }
    }

    if (others.size() > 2)
    {
        const std::vector<BgmInstrument> instruments = ParsePrgFile(archive.File(others[2]));
        for (std::size_t i = 0; i < global_instruments_.size() && i < instruments.size(); i++)
        {
            global_instruments_[i] = instruments[i];
        }
    }

    for (const SbnArchive::InitBank& bank : archive.InitBanks())
    {
        LoadBank(bank.file, bank.bank, bank.bank_set);
    }

    InitPlayer(players_[0], 1, 0, {0, 3, -1, -1});
    InitPlayer(players_[1], 2, 2, {2, -1, -1, -1});
}

int SbnPlayer::AddWave(const BkFile& file, std::span<const uint8_t> bytes, uint32_t file_place,
                       const BkInstrument& instrument)
{
    (void)file;
    Wave wave;
    wave.data = instrument.wave != 0 ? AudioMemory::kImageBase + file_place + instrument.wave : 0;
    wave.length = instrument.wave_size;
    wave.book = AudioMemory::kImageBase + file_place + instrument.book;
    wave.book_size = instrument.book_size;
    wave.loop_state = instrument.loop_state != 0 ? AudioMemory::kImageBase + file_place + instrument.loop_state : 0;
    wave.loop_start = instrument.loop_start;
    wave.loop_end = instrument.loop_end;
    wave.loop_count = instrument.loop_count;
    wave.key_base = instrument.key_base;
    wave.type = instrument.type;
    wave.pitch_ratio = static_cast<float>(instrument.sample_rate) / static_cast<float>(config_.output_rate);
    wave.file = bytes;
    wave.envelopes = instrument.envelopes;
    waves_.push_back(wave);

    return static_cast<int>(waves_.size()) - 1;
}

void SbnPlayer::LoadBank(uint16_t file, int bank, int bank_set)
{
    const std::span<const uint8_t> bytes = archive_->File(file);
    const BkFile parsed = BkFile::Parse(bytes);
    const uint32_t place = config_.archive_place + archive_->Entries()[file].offset;
    auto& slots = bank_sets_[static_cast<std::size_t>(bank_set)][static_cast<std::size_t>(bank)];
    for (std::size_t i = 0; i < parsed.slots.size(); i++)
    {
        const int32_t index = parsed.slots[i];
        slots[i] = index >= 0 ? AddWave(parsed, bytes, place, parsed.instruments[static_cast<std::size_t>(index)])
                              : default_wave_;
    }
}

int SbnPlayer::WaveOf(int bank, int patch, Envelope& envelope) const
{
    // The bank's high nibble picks the set. The patch holds the set's bank and the bank's instrument.
    const auto set = static_cast<std::size_t>((bank & 0x70) >> 4);
    const auto group = static_cast<std::size_t>((patch >> 4) & 0xF);
    const auto slot = static_cast<std::size_t>(patch & 0xF);
    const int index = bank_sets_[set][group][slot];
    const Wave& wave = waves_[static_cast<std::size_t>(index)];
    const uint32_t which = static_cast<uint32_t>(bank) & 3;
    if (wave.envelopes != 0 && !wave.file.empty() && which < wave.file[wave.envelopes])
    {
        const uint8_t* const entry = wave.file.data() + wave.envelopes + 4 + which * 4;
        envelope.press = {wave.file, -1, 0, wave.envelopes + xsflib::LoadBe16(entry)};
        envelope.release = {wave.file, -1, 0, wave.envelopes + xsflib::LoadBe16(entry + 2)};
    }
    else
    {
        envelope.press = {{}, -1, 0, 0};
        envelope.release = {{}, -1, 0, 4};
    }

    return index;
}

uint8_t SbnPlayer::EnvelopeByte(const EnvelopePlace& place) const
{
    if (place.custom_player >= 0)
    {
        const auto& custom = players_[static_cast<std::size_t>(place.custom_player)]
                                 .custom_envelopes[static_cast<std::size_t>(place.custom)];
        const uint16_t value = custom[place.at / 2];

        return static_cast<uint8_t>((place.at & 1) == 0 ? value >> 8 : value);
    }

    const std::span<const uint8_t> bytes = place.bytes.empty() ? config_.tables.default_envelope : place.bytes;

    return place.at < bytes.size() ? bytes[place.at] : kEndCommand;
}

uint8_t SbnPlayer::ReadEnvelope(EnvelopePlace& place) const
{
    const uint8_t value = EnvelopeByte(place);
    place.at++;

    return value;
}

float SbnPlayer::PitchRatio(int32_t tuning) const
{
    const std::vector<float>& table = config_.tables.tune_scaling;
    if (tuning >= 0)
    {
        return table[static_cast<std::size_t>(tuning & 0x7F)] *
               table[static_cast<std::size_t>(((tuning & 0xF80) >> 7) + 128)];
    }

    tuning = -tuning;

    return table[static_cast<std::size_t>((tuning & 0x7F) + 160)] *
           table[static_cast<std::size_t>(((tuning & 0x3F80) >> 7) + 288)];
}

void SbnPlayer::InitPlayer(Player& player, int priority, int bus, std::array<int8_t, 4> effects)
{
    SetTickResolution(player, kDefaultStep, kDefaultTicksPerBeat);
    player.bus_volume = 0x8000;
    player.master_tempo = kDefaultBpm * kTempoScale;
    player.master_volume = 0x7F << 24;
    player.priority = static_cast<uint8_t>(priority);
    player.bus = static_cast<uint8_t>(bus);
    player.effect_indices = effects;
    player.polyphony_counts = {0, 1, 0, 0, 0, 2, 3, 4};
    player.fade.envelope_volume = 0x7FFF << 16;
    player.fade.envelope_target = 0x7FFF;
    player.state = kStateIdle;
}

void SbnPlayer::SetTickResolution(Player& player, int32_t step, uint32_t ticks_per_beat)
{
    uint32_t frames_per_tick = kFramesPerMinute / ticks_per_beat;
    frames_per_tick = std::clamp<uint32_t>(frames_per_tick, 80000, 500000);
    if (frames_per_tick < static_cast<uint32_t>(step))
    {
        step = static_cast<int32_t>(frames_per_tick);
    }

    player.update_step = step;
    player.tick_interval = static_cast<int32_t>(frames_per_tick);
    player.update_counter = static_cast<int32_t>(frames_per_tick);
    player.max_tempo = frames_per_tick / kUpdateScale;
}

bool SbnPlayer::LoadSong(int player, int song)
{
    const auto& songs = archive_->Songs();
    if (song < 0 || static_cast<std::size_t>(song) >= songs.size())
    {
        return false;
    }

    // The list ends with an entry that names no file.
    const SbnArchive::Song& entry = songs[static_cast<std::size_t>(song)];
    if (entry.bgm >= archive_->Entries().size())
    {
        return false;
    }

    Player& held = players_[static_cast<std::size_t>(player)];
    held.file = archive_->File(entry.bgm);
    held.parsed = std::make_shared<const BgmFile>(BgmFile::Parse(held.file));
    held.song = song;
    for (std::size_t i = 0; i < entry.banks.size(); i++)
    {
        if (entry.banks[i] != 0 && archive_->Entries()[entry.banks[i]].format == kSbnFormatBank)
        {
            LoadBank(entry.banks[i], static_cast<int>(i), 0);
        }
    }

    return true;
}

void SbnPlayer::StartSong(int player, int variation, int duration, int start_volume, int final_volume)
{
    Player& held = players_[static_cast<std::size_t>(player)];
    if (duration != 0)
    {
        duration = std::clamp(duration, 250, 10000);
    }

    start_volume = std::min(start_volume, 0x7F);
    const int first = start_volume != 0 ? (start_volume << 8) | 0xFF : 0;
    final_volume = std::min(final_volume, 0x7F);
    const int last = final_volume != 0 ? (final_volume << 8) | 0xFF : 0x7FFF;
    Fade& fade = held.fade;
    fade.base_volume = first << 16;
    fade.base_target = static_cast<int16_t>(last);
    if (duration != 0)
    {
        fade.base_ticks = static_cast<uint16_t>((duration * 1000) / kFrameMicroseconds);
        fade.base_step = ((last << 16) - fade.base_volume) / fade.base_ticks;
    }
    else
    {
        fade.base_ticks = 1;
        fade.base_step = 0;
    }

    fade.envelope_target = 0x7FFF;
    fade.envelope_ticks = 1;
    SetTickResolution(held, kDefaultStep, config_.tables.ticks_per_beat[held.parsed->timing_preset & 7]);
    if (variation < 0 || variation > 3 || held.parsed->CompositionOffset(variation) == 0)
    {
        variation = 0;
    }

    held.composition_start = held.parsed->CompositionOffset(variation);
    held.composition = held.composition_start;
    held.active = true;
    Initialize(player);
}

void SbnPlayer::Stop(int player)
{
    UpdateStop(player);
}

bool SbnPlayer::Playing(int player) const
{
    return players_[static_cast<std::size_t>(player)].active;
}

bool SbnPlayer::Busy() const
{
    return std::ranges::any_of(players_, [](const Player& player) { return player.active; }) ||
           std::ranges::any_of(voices_, [](const Voice& voice) { return voice.playing; });
}

void SbnPlayer::SetProximityMix(int player, uint32_t mix)
{
    Player& held = players_[static_cast<std::size_t>(player)];
    held.proximity_id = static_cast<int32_t>(mix & 0xFF);
    held.proximity_volume = static_cast<uint8_t>((mix >> 24) & 0xFF);
}

void SbnPlayer::UpdateBusVolumes(Player& player)
{
    const auto volume = static_cast<uint16_t>(((static_cast<uint32_t>(player.fade.base_volume) >> 16) *
                                               (static_cast<uint32_t>(player.fade.envelope_volume) >> 16)) >>
                                              15);
    for (const int8_t bus : player.effect_indices)
    {
        if (bus < 0)
        {
            return;
        }

        SetBusVolume(bus, volume, player.bus_volume);
    }
}

void SbnPlayer::SetBusVolume(int bus, uint16_t volume, int32_t bus_volume)
{
    buses_[static_cast<std::size_t>(bus)].gain =
        static_cast<uint16_t>((static_cast<uint32_t>(volume) * static_cast<uint32_t>(bus_volume)) / 0x8000 & 0x7FFF);
}

void SbnPlayer::UpdateFade(int index)
{
    Player& player = players_[static_cast<std::size_t>(index)];
    player.fade.base_ticks--;
    if (player.fade.base_ticks != 0)
    {
        player.fade.base_volume += player.fade.base_step;
    }
    else
    {
        player.fade.base_volume = static_cast<int32_t>(player.fade.base_target) << 16;
        if (player.fade.base_volume == 0)
        {
            UpdateStop(index);
        }
    }

    UpdateBusVolumes(player);
}

void SbnPlayer::RunChunkClients()
{
    BeginChunk();
    for (const int index : {1, 0})
    {
        Player& player = players_[static_cast<std::size_t>(index)];
        if (index == 0 && player.fade.envelope_ticks != 0)
        {
            player.fade.envelope_ticks--;
            if (player.fade.envelope_ticks != 0)
            {
                player.fade.envelope_volume += player.fade.envelope_step;
            }
            else
            {
                player.fade.envelope_step = 0;
                player.fade.envelope_volume = static_cast<int32_t>(player.fade.envelope_target) << 16;
            }

            if (player.fade.base_ticks == 0)
            {
                UpdateBusVolumes(player);
            }
            else
            {
                UpdateFade(index);
            }
        }
        else if (player.fade.base_ticks != 0)
        {
            UpdateFade(index);
        }

        if (player.song >= 0)
        {
            player.counter++;
        }

        player.update_counter -= player.update_step;
        if (player.update_counter <= 0)
        {
            player.update_counter += player.tick_interval;
            RunPlayer(index);
        }
    }

    UpdateVoices();
}

void SbnPlayer::BeginChunk()
{
    if (request_delay_pending_)
    {
        if (request_delay_side_ == kDelayLeft || request_delay_side_ == kDelayRight)
        {
            delay_count_ = std::min<int32_t>(request_delay_time_, 4);
            if (request_delay_time_ < 2)
            {
                delayed_channel_ = kDelayNone;
            }

            std::vector<uint8_t> zeros(4 * kChunk * 2, 0);
            build_memory_->Write(delay_main_, zeros.data(), zeros.size());
            build_memory_->Write(delay_aux_, zeros.data(), zeros.size());
            delayed_bus_ = request_delay_bus_;
            delayed_channel_ = request_delay_side_;
        }
        else
        {
            delayed_bus_ = 0;
            delayed_channel_ = kDelayNone;
        }

        delay_counter_ = 0;
        request_delay_pending_ = false;
    }

    for (const int bus : {0, 1, 2, 3})
    {
        EffectChange& change = effect_changes_[static_cast<std::size_t>(bus)];
        if (change.changed)
        {
            Bus& held = buses_[static_cast<std::size_t>(bus)];
            held.effect_type = change.type;
            LoadEffect(held, held.effects[0], change.type);
            LoadEffect(held, held.effects[1], change.type);
            change.changed = false;
        }
    }

    for (int i = 0; i < kVoices; i++)
    {
        Voice& voice = voices_[static_cast<std::size_t>(i)];
        const uint8_t flags = voice.sync_flags;
        if (voice.done_pending)
        {
            SynthStopVoice(i);
            voice.done_pending = false;
            voice.has_commands = false;
            voice.priority = 0;
        }

        if ((flags & kSyncAll) != 0)
        {
            StartEnvelope(voice);
            SynthStartVoice(i);
            voice.priority = voice.client_priority;
        }
        else
        {
            if ((flags & kSyncPitch) != 0)
            {
                voice.ratio = voice.pitch_ratio;
            }

            if ((flags & kSyncParams) != 0)
            {
                SynthSetMixer(i, voice.volume, voice.delta, voice.pan, voice.reverb);
            }
            else if ((flags & kSyncPanMix) != 0)
            {
                SynthSetPanAndMix(i, voice.pan, voice.reverb);
            }
        }

        voice.sync_flags = 0;
    }
}

void SbnPlayer::RunPlayer(int index)
{
    Player& player = players_[static_cast<std::size_t>(index)];
    player.random_1 = (player.random_1 & 0xFFFF) + (player.counter & 0xFFFF) + (player.frames & 0xFFFF);
    player.random_2 = (player.random_2 & 0xFFFF) + ((player.counter << 4) & 0xFFFF) + ((player.frames >> 4) & 0xFFFF);
    bool more = true;
    while (more)
    {
        switch (player.state)
        {
        case kStateActive:
            if (!player.paused)
            {
                UpdatePlaying(index);
                more = player.state != kStateActive;
            }
            else
            {
                more = false;
            }
            break;

        case kStateFetch:
            ReadComposition(player);
            break;

        case kStateInit:
            Initialize(index);
            more = false;
            break;

        case kStateStop:
            UpdateStop(index);
            break;

        default:
            more = false;
            break;
        }
    }
}

void SbnPlayer::Initialize(int index)
{
    Player& player = players_[static_cast<std::size_t>(index)];
    for (int i = 0; i < kTracks; i++)
    {
        Track& track = player.tracks[static_cast<std::size_t>(i)];
        const uint8_t keep_first = track.first_voice;
        const uint8_t keep_last = track.last_voice;
        track = Track{};
        track.first_voice = keep_first;
        track.last_voice = keep_last;
        track.instrument_volume = 0x7FFF << 16;
        track.proximity_volume = 0x7FFF << 16;
        track.bus = player.bus;
    }

    for (Note& note : player.notes)
    {
        note = Note{};
    }

    ResetAllVoices(player);
    player.master_tempo = kDefaultBpm * kTempoScale;
    player.master_tempo_bpm = kDefaultBpm;
    player.master_volume = 0x7F << 24;
    player.tempo_target = 0;
    player.pitch_shift = 0;
    player.detune = 0;
    player.volume_ticks = 0;
    player.volume_target = 0;
    player.volume_step = 0;
    player.proximity_value = 0;
    player.proximity_id = 0;
    player.proximity_volume = 0;
    player.loop_ends = {};
    player.loop_counters = {};
    player.loop_depth = 0;
    player.condition_flags = 0;
    player.first_link_mute = true;
    player.custom_writing = 0;
    player.playback_rate = 1.0F;
    for (std::size_t i = 0; i < player.custom_envelopes.size(); i++)
    {
        player.custom_envelopes[i].fill(0xFF00);
        player.custom_write[i] = 0;
    }

    player.effect_values = {};
    player.paused = false;
    player.counter = 0;
    player.loop_labels.fill(player.composition);
    for (uint32_t at = player.composition; at + 4 <= player.file.size(); at += 4)
    {
        const uint32_t command = xsflib::LoadBe32(player.file.data() + at);
        if (command == 0)
        {
            break;
        }

        if ((command >> 28) == 3)
        {
            player.loop_labels[command & 0x1F] = at + 4;
        }
    }

    player.state = kStateFetch;
}

void SbnPlayer::ReadComposition(Player& player)
{
    player.tempo_step = 0;
    player.tempo_ticks = 0;
    bool reading = true;
    while (reading)
    {
        const uint32_t command = xsflib::LoadBe32(player.file.data() + player.composition);
        player.composition += 4;
        if (command == 0)
        {
            player.state = kStateStop;
            break;
        }

        switch (command >> 28)
        {
        case 1:
            LoadPhrase(player, command);
            player.state = kStateActive;
            reading = false;
            break;

        case 3:
            break;

        case 5:
            EndCompositionLoop(player, command);
            break;

        case 6:
            if ((player.condition_flags & 1) == 0)
            {
                EndCompositionLoop(player, command);
            }
            break;

        case 7:
            if ((player.condition_flags & 1) != 0)
            {
                EndCompositionLoop(player, command);
            }
            break;

        default:
            reading = false;
            break;
        }
    }
}

void SbnPlayer::EndCompositionLoop(Player& player, uint32_t command)
{
    const uint32_t label = command & 0x1F;
    const auto count = static_cast<uint8_t>((command >> 5) & 0x7F);
    uint32_t depth = player.loop_depth;
    if (player.loop_ends[depth] == 0)
    {
        player.loop_ends[depth] = player.composition;
        player.loop_counters[depth] = count;
        player.composition = player.loop_labels[label];
    }
    else if (player.loop_ends[depth] == player.composition)
    {
        RepeatCompositionLoop(player, depth, label);
    }
    else if (depth < 3)
    {
        depth++;
        player.loop_ends[depth] = player.composition;
        player.loop_counters[depth] = count;
        player.composition = player.loop_labels[label];
    }

    player.loop_depth = depth;
}

// Ends a pass of the loop at `depth`. Jumps back to the label of the loop, or leaves the loop after the last pass. A
// count of 0 repeats the loop without end.
void SbnPlayer::RepeatCompositionLoop(Player& player, uint32_t& depth, uint32_t label)
{
    if (player.loop_counters[depth] != 0)
    {
        player.loop_counters[depth]--;
        if (player.loop_counters[depth] == 0)
        {
            player.loop_ends[depth] = 0;
            if (depth > 0)
            {
                depth--;
            }

            return;
        }
    }

    player.composition = player.loop_labels[label];
}

void SbnPlayer::LoadPhrase(Player& player, uint32_t command)
{
    uint8_t next_voice = 0;
    bool found_link = false;
    player.phrase = player.composition_start + ((command & 0xFFFF) << 2);
    for (int i = 0; i < kTracks; i++)
    {
        Track& track = player.tracks[static_cast<std::size_t>(i)];
        const uint32_t info = xsflib::LoadBe32(player.file.data() + player.phrase + static_cast<uint32_t>(i) * 4);
        const uint32_t offset = info >> 16;
        track.position = offset;
        if (offset == 0)
        {
            continue;
        }

        if ((info & 0x100) != 0)
        {
            track.position = 0;
            continue;
        }

        track.polyphony = static_cast<uint8_t>((info >> 13) & 7);
        track.drums = ((info >> 7) & 1) != 0;
        const auto linked = static_cast<uint8_t>((info >> 9) & 0xF);
        track.linked_track = 0;
        if (linked != 0)
        {
            if (linked - 1 < i)
            {
                const Track& other = player.tracks[static_cast<std::size_t>(linked - 1)];
                track.voice_count = other.voice_count;
                track.first_voice = other.first_voice;
                track.last_voice = other.last_voice;
                track.position = offset + player.phrase;
                track.delay = 1;
                track.linked_track = linked;
                if (player.first_link_mute)
                {
                    track.muted = true;
                }

                found_link = true;
            }
            else
            {
                track.position = 0;
            }
        }
        else
        {
            const uint8_t count = player.polyphony_counts[track.polyphony];
            track.voice_count = count;
            track.first_voice = next_voice;
            next_voice = static_cast<uint8_t>(next_voice + count);
            track.last_voice = next_voice;
            track.position = offset + player.phrase;
            track.delay = 1;
        }
    }

    if (found_link)
    {
        player.first_link_mute = false;
    }
}

void SbnPlayer::UpdateStop(int index)
{
    Player& player = players_[static_cast<std::size_t>(index)];
    player.paused = false;
    player.song = -1;
    player.active = false;
    for (Track& track : player.tracks)
    {
        track.position = 0;
    }

    ResetAllVoices(player);
    player.state = kStateIdle;
    player.update_step = kDefaultStep;
}

uint8_t SbnPlayer::ReadTrackByte(Player& player, Track& track)
{
    const uint8_t value = track.position < player.file.size() ? player.file[track.position] : uint8_t{0};
    track.position++;
    if (track.detour_length != 0)
    {
        track.detour_length--;
        if (track.detour_length == 0)
        {
            track.position = track.saved_position;
        }
    }

    return value;
}

void SbnPlayer::UpdatePlaying(int index)
{
    Player& player = players_[static_cast<std::size_t>(index)];
    const bool fading = StepPlayerRamps(player);
    bool finished = false;
    player.volume_changed = false;
    for (int t = 0; t < kTracks; t++)
    {
        Track& track = player.tracks[static_cast<std::size_t>(t)];
        if (track.position == 0)
        {
            continue;
        }

        StepTrackRamps(track, fading || player.volume_changed);
        track.delay--;
        if (track.delay <= 0 && RunTrackCommands(index, t, track))
        {
            finished = true;
        }

        UpdateTrackNotes(player, track);
    }

    if (finished)
    {
        player.state = kStateFetch;
    }
}

// Steps the tempo ramp and the volume ramp of a player. Returns whether the volume ramps.
bool SbnPlayer::StepPlayerRamps(Player& player)
{
    if (player.tempo_ticks != 0)
    {
        player.tempo_ticks--;
        if (player.tempo_ticks == 0)
        {
            player.master_tempo = player.tempo_target;
            player.tempo_target = 0;
            player.tempo_step = 0;
        }
        else
        {
            player.master_tempo += player.tempo_step;
        }

        player.update_step =
            static_cast<int32_t>(kUpdateScale * static_cast<uint32_t>(player.master_tempo) / kTempoScale);
    }

    if (player.volume_ticks == 0)
    {
        return false;
    }

    player.volume_ticks--;
    if (player.volume_ticks == 0)
    {
        player.master_volume = player.volume_target;
        player.volume_target = 0;
        player.volume_step = 0;
    }
    else
    {
        player.master_volume += player.volume_step;
    }

    return true;
}

// Clears the change flags of a track and steps the volume ramps of the track.
void SbnPlayer::StepTrackRamps(Track& track, bool changed_volume)
{
    track.changed_pan = false;
    track.changed_reverb = false;
    track.changed_tune = false;
    track.changed_volume = changed_volume;
    if (track.volume_ticks != 0)
    {
        track.volume_ticks--;
        track.instrument_volume =
            track.volume_ticks == 0 ? track.volume_target : track.instrument_volume + track.volume_step;
        track.changed_volume = true;
    }

    if (track.proximity_ticks != 0)
    {
        track.proximity_ticks--;
        track.proximity_volume =
            track.proximity_ticks == 0 ? track.proximity_target << 16 : track.proximity_volume + track.proximity_step;
        track.changed_volume = true;
    }
}

// Runs the commands of a track until one sets a delay. Returns true when the track data ends.
bool SbnPlayer::RunTrackCommands(int index, int t, Track& track)
{
    Player& player = players_[static_cast<std::size_t>(index)];
    uint8_t search_from = track.first_voice;
    while (track.delay == 0)
    {
        const uint32_t command_place = track.position;
        const uint8_t opcode = ReadTrackByte(player, track);
        if (opcode == 0)
        {
            if (track.previous_position == 0)
            {
                return true;
            }

            track.position = track.previous_position;
            track.previous_position = 0;
        }
        else if (opcode < 0x78)
        {
            track.delay = opcode;
        }
        else if (opcode < 0x80)
        {
            track.delay = ((opcode & 7) << 8) + ReadTrackByte(player, track) + 0x78;
        }
        else if (opcode < 0xD4)
        {
            StartNote(index, t, track, opcode, command_place, search_from);
        }
        else
        {
            RunCommand(index, track, opcode);
        }
    }

    return false;
}

} // namespace ultralib::n64
