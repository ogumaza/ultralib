// SPDX-License-Identifier: MIT

// The commands of the SBN engine's tracks, and the notes that they start on the voices of a track.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

#include "sbn_player_constants.h"
#include "ultralib/n64/sbn.h"
#include "ultralib/n64/sbn_player.h"

namespace ultralib::n64
{

namespace
{

using sbn_player_constants::kDelayNone;
using sbn_player_constants::kEnvKeyReleased;
using sbn_player_constants::kEnvVolumeChanged;
using sbn_player_constants::kSyncAll;
using sbn_player_constants::kSyncPanMix;
using sbn_player_constants::kSyncPitch;
using sbn_player_constants::kTempoScale;
using sbn_player_constants::kUpdateScale;

// The arguments of commands 0xE0 to 0xFF, from the same table of the game's code as the reader's.
constexpr std::array<uint8_t, 32> kArgumentCounts = {2, 1, 1, 1, 4, 3, 2, 0, 2, 1, 1, 1, 1, 1, 1, 2,
                                                     3, 1, 1, 0, 2, 1, 3, 1, 0, 0, 0, 0, 3, 3, 3, 3};

int32_t Volume8To32(uint8_t value)
{
    const int32_t volume = value & 0x7F;

    return volume != 0 ? volume << 24 : 0;
}

uint8_t RandomPan(uint32_t seed, uint8_t pan, uint8_t amplitude)
{
    const auto value = static_cast<int32_t>(seed);
    const int32_t parity = ((value >> 7) + (value >> 2)) & 1;
    const int32_t random = ((value >> 8) & 0x3F) + ((value << 4) & 0xC0);
    int32_t result = parity != 0 ? pan + ((amplitude * random) >> 8) : pan - ((amplitude * random) >> 8);
    result = std::clamp(result, 0, 0x7F);

    return static_cast<uint8_t>(result);
}

int16_t RandomPitch(uint32_t seed, int32_t pitch, uint8_t amplitude)
{
    const auto value = static_cast<int32_t>(seed);
    const int32_t parity = ((value >> 4) + (value >> 1)) & 1;
    const int32_t random = ((value >> 6) & 0xF) + ((value << 2) & 0xF0);

    return static_cast<int16_t>(parity != 0 ? pitch + ((amplitude * 5 * random) >> 8)
                                            : pitch - ((amplitude * 5 * random) >> 8));
}

uint8_t RandomVolume(uint32_t seed, uint8_t volume, uint8_t amplitude)
{
    const auto value = static_cast<int32_t>(seed);
    const int32_t random = ((value >> 8) & 0x1F) + (value & 0xE0);

    return static_cast<uint8_t>(volume * (0x8000 - amplitude * random));
}

uint8_t RandomReverb(uint32_t seed, uint8_t reverb, uint8_t amplitude)
{
    const auto value = static_cast<int32_t>(seed);
    const int32_t random = ((value >> 7) & 7) + ((value << 3) & 0xF8);

    return static_cast<uint8_t>(reverb * (0x8000 - amplitude * random));
}

} // namespace

// Reads the velocity and the length of a note command, then starts the note on a voice of the track.
void SbnPlayer::StartNote(int index, int t, Track& track, uint8_t opcode, uint32_t command_place, uint8_t& search_from)
{
    Player& player = players_[static_cast<std::size_t>(index)];
    const int32_t key = opcode & 0x7F;
    const uint8_t velocity = ReadTrackByte(player, track);
    int32_t length = ReadTrackByte(player, track);
    if (length >= 0xC0)
    {
        length = ((length & 0x3F) << 8) + ReadTrackByte(player, track) + 0xC0;
    }

    uint8_t voice_index = 0;
    if (track.muted || !AcquireVoice(player, track, search_from, voice_index))
    {
        return;
    }

    Voice& voice = voices_[voice_index];
    Note& note = player.notes[voice_index];
    note.tremolo_depth = 0;
    note.velocity = velocity > 0 ? velocity + 1 : 0;
    note.length = length;
    const int32_t level =
        (((player.master_volume >> 21) * (track.instrument_volume >> 21) * (track.proximity_volume >> 21)) >> 20);
    if (track.drums)
    {
        SetUpDrumNote(player, track, key, level, voice, note);
    }
    else
    {
        SetUpInstrumentNote(index, track, key, level, voice, note);
    }

    voice.wave = note.wave;
    voice.pitch_ratio = note.pitch_ratio;
    voice.bus = track.bus;
    voice.track = t;
    if (note.length > 1)
    {
        note.pending_tick = true;
        note.tremolo_depth = track.tremolo_depth;
        note.tremolo_phase = 0;
        note.tremolo_delay = track.tremolo_delay;
        voice.sync_flags = kSyncAll;
        voice.priority = player.priority;
        voice.client_priority = voice.priority;
    }

    voice.host_track = index * kTracks + t;
    if (on_note)
    {
        on_note({index, t, static_cast<uint8_t>(key), velocity, length, track.drums, command_place, voice_index});
    }
}

// Finds a voice for a new note of a track. Uses the next free voice of the track, or frees one by the rules of the
// polyphony. Returns false when no voice is free.
bool SbnPlayer::AcquireVoice(Player& player, const Track& track, uint8_t& search_from, uint8_t& voice_index)
{
    for (voice_index = search_from; voice_index < track.last_voice; voice_index++)
    {
        search_from++;
        if (voices_[voice_index].priority == 0)
        {
            return true;
        }
    }

    if (track.polyphony >= 5)
    {
        return StealVoice(player, track, voice_index);
    }

    voice_index = track.first_voice;
    player.notes[voice_index].length = 0;
    if (voices_[voice_index].priority <= player.priority)
    {
        ResetVoice(voice_index);

        return true;
    }

    return false;
}

// Frees a voice of a track of polyphony 5 or more: one of a lower priority, then one whose note has ended, then the
// one whose note has the shortest length left.
bool SbnPlayer::StealVoice(Player& player, const Track& track, uint8_t& voice_index)
{
    for (voice_index = track.first_voice; voice_index < track.last_voice; voice_index++)
    {
        if (voices_[voice_index].priority < player.priority)
        {
            ResetVoice(voice_index);

            return true;
        }
    }

    for (voice_index = track.first_voice; voice_index < track.last_voice; voice_index++)
    {
        if (voices_[voice_index].priority == player.priority && player.notes[voice_index].length == 0)
        {
            ResetVoice(voice_index);

            return true;
        }
    }

    bool acquired = false;
    int32_t shortest = 0xFFFF;
    for (uint8_t other = track.first_voice; other < track.last_voice; other++)
    {
        const Note& held = player.notes[other];
        if (voices_[other].priority == player.priority && !held.pending_tick && held.length < shortest)
        {
            shortest = held.length;
            voice_index = other;
            acquired = true;
        }
    }

    if (acquired)
    {
        player.notes[voice_index].length = 0;
        ResetVoice(voice_index);
    }

    return acquired;
}

// Sets the wave, the volume, the pitch, the pan and the reverb of a drum note from the drum of its key.
void SbnPlayer::SetUpDrumNote(const Player& player, const Track& track, int32_t key, int32_t level, Voice& voice,
                              Note& note)
{
    const BgmDrum* drum = nullptr;
    if (key < 72)
    {
        drum = &global_drums_[static_cast<std::size_t>(key)];
    }
    else if (!player.parsed->drums.empty())
    {
        const auto number = static_cast<std::size_t>(key - 72);
        drum = number < player.parsed->drums.size() ? &player.parsed->drums[number] : &player.parsed->drums[0];
    }
    else
    {
        drum = &global_drums_[0];
    }

    note.wave = WaveOf(drum->bank_patch >> 8, drum->bank_patch & 0xFF, voice.envelope);
    const Wave& wave = waves_[static_cast<std::size_t>(note.wave)];
    note.volume = drum->random_volume != 0
                      ? note.velocity * RandomVolume(player.random_1, drum->volume, drum->random_volume)
                      : note.velocity * drum->volume;
    voice.client_volume = static_cast<int16_t>((level * (track.volume * note.volume)) >> 16);
    note.detune = drum->key_base + track.coarse_detune + track.fine_detune - wave.key_base;
    int32_t tune = note.detune + track.detune + player.detune;
    if (drum->random_tune != 0)
    {
        note.rand_detune = RandomPitch(player.random_1, tune, drum->random_tune);
        tune = note.rand_detune;
    }

    note.pitch_ratio = PitchRatio(tune) * wave.pitch_ratio;
    voice.pan = drum->random_pan != 0 ? RandomPan(player.random_1, static_cast<uint8_t>(drum->pan), drum->random_pan)
                                      : static_cast<uint8_t>(drum->pan);
    voice.reverb =
        drum->random_reverb != 0 ? RandomReverb(player.random_1, drum->reverb, drum->random_reverb) : drum->reverb;
}

// Sets the wave, the volume, the pitch, the pan, the reverb and the envelope of a note from the instrument of its
// track.
void SbnPlayer::SetUpInstrumentNote(int index, const Track& track, int32_t key, int32_t level, Voice& voice, Note& note)
{
    const Player& player = players_[static_cast<std::size_t>(index)];
    note.volume = (level * (track.volume * note.velocity)) >> 9;
    voice.client_volume = static_cast<int16_t>(note.volume);
    note.wave = track.wave >= 0 ? track.wave : default_wave_;
    const Wave& wave = waves_[static_cast<std::size_t>(note.wave)];
    note.detune = key * 100 + track.coarse_detune + player.pitch_shift + track.fine_detune - wave.key_base;
    note.pitch_ratio = PitchRatio(note.detune + track.detune + player.detune) * wave.pitch_ratio;
    voice.pan = track.random_pan != 0 ? RandomPan(player.random_1, track.pan, track.random_pan) : track.pan;
    voice.reverb = track.reverb;
    if (track.press_override != 0)
    {
        voice.envelope.press = {{}, index, track.press_override - 1, 0};
    }
    else
    {
        voice.envelope.press = track.envelope.press;
    }

    voice.envelope.release = track.envelope.release;
}

// Steps the notes that a track plays on the voices of its range.
void SbnPlayer::UpdateTrackNotes(Player& player, const Track& track)
{
    if (track.muted)
    {
        return;
    }

    for (uint8_t voice_index = track.first_voice; voice_index < track.last_voice; voice_index++)
    {
        Voice& voice = voices_[voice_index];
        if (voice.priority != player.priority)
        {
            continue;
        }

        Note& note = player.notes[voice_index];
        if (!note.pending_tick)
        {
            UpdateNote(player, track, voice, note);
        }

        note.pending_tick = false;
    }
}

// Counts down the length of a note and applies the changes of the track to the voice of the note.
void SbnPlayer::UpdateNote(const Player& player, const Track& track, Voice& voice, Note& note)
{
    if (note.length > 0)
    {
        note.length--;
        if (note.length == 0)
        {
            voice.envelope_flags |= kEnvKeyReleased;
        }
    }

    const int32_t level =
        (((player.master_volume >> 21) * (track.instrument_volume >> 21) * (track.proximity_volume >> 21)) >> 20);
    if (track.drums)
    {
        UpdateDrumNote(player, track, level, voice, note);
    }
    else
    {
        UpdateInstrumentNote(player, track, level, voice, note);
    }
}

// Applies the changes of tune and volume of a track to a drum note.
void SbnPlayer::UpdateDrumNote(const Player& player, const Track& track, int32_t level, Voice& voice, Note& note)
{
    if (track.changed_tune || player.detune != 0)
    {
        SetNotePitch(voice, note, (note.detune + note.rand_detune) + track.detune + player.detune);
    }

    if (track.changed_volume)
    {
        voice.client_volume = static_cast<int16_t>((level * (track.volume * note.volume)) >> 16);
        voice.envelope_flags |= kEnvVolumeChanged;
    }
}

// Steps the tremolo of an instrument note and applies the changes of tune, volume, pan and reverb of the track.
void SbnPlayer::UpdateInstrumentNote(const Player& player, const Track& track, int32_t level, Voice& voice, Note& note)
{
    if (note.tremolo_depth != 0)
    {
        if (note.tremolo_delay != 0)
        {
            note.tremolo_delay--;
        }
        else
        {
            note.tremolo_phase = static_cast<uint8_t>(note.tremolo_phase + track.tremolo_rate);
            const auto shape = static_cast<uint8_t>((note.tremolo_phase << 2) + 3);
            const uint8_t quadrant = note.tremolo_phase >> 6;
            int32_t detune = (quadrant == 1 || quadrant == 3) ? static_cast<uint8_t>(~shape) + 1 : shape;
            detune = (detune * track.tremolo_depth) >> 8;
            if (quadrant >= 2)
            {
                detune = -detune;
            }

            SetNotePitch(voice, note, detune + note.detune + track.detune + player.detune);
        }
    }
    else if (track.changed_tune || player.detune != 0)
    {
        SetNotePitch(voice, note, note.detune + track.detune + player.detune);
    }

    if (track.changed_volume)
    {
        note.volume = (level * (track.volume * note.velocity)) >> 9;
        voice.client_volume = static_cast<int16_t>(note.volume);
        voice.envelope_flags |= kEnvVolumeChanged;
        voice.pan = track.pan;
        voice.reverb = track.reverb;
    }
    else if (track.changed_pan || track.changed_reverb)
    {
        voice.pan = track.pan;
        voice.reverb = track.reverb;
        voice.sync_flags |= kSyncPanMix;
    }
}

// Sets the pitch of a note from a tuning in cents. Marks the voice for a pitch sync when the pitch changes.
void SbnPlayer::SetNotePitch(Voice& voice, Note& note, int32_t tuning)
{
    const Wave& wave = waves_[static_cast<std::size_t>(note.wave)];
    note.pitch_ratio = PitchRatio(tuning) * wave.pitch_ratio;
    if (voice.pitch_ratio != note.pitch_ratio)
    {
        voice.pitch_ratio = note.pitch_ratio;
        voice.sync_flags |= kSyncPitch;
    }
}

void SbnPlayer::RunCommand(int index, Track& track, uint8_t opcode)
{
    Player& player = players_[static_cast<std::size_t>(index)];
    if (opcode < 0xE0)
    {
        return;
    }

    const uint8_t count = kArgumentCounts[static_cast<std::size_t>(opcode - 0xE0)];
    std::array<uint8_t, 4>& args = player.args;
    for (uint8_t i = 0; i < count; i++)
    {
        args[i] = ReadTrackByte(player, track);
    }

    const auto word = [&](std::size_t at)
    {
        return static_cast<uint16_t>((args[at] << 8) | args[at + 1]);
    };

    const auto to_tempo = [&](uint32_t bpm)
    {
        auto value = static_cast<uint32_t>(static_cast<float>(bpm) * player.playback_rate);
        if (player.max_tempo < value)
        {
            value = player.max_tempo;
        }
        else if (value == 0)
        {
            value = 1;
        }

        return static_cast<int32_t>(value * kTempoScale);
    };

    switch (opcode)
    {
    case 0xE0:
        player.master_tempo_bpm = word(0);
        player.master_tempo = to_tempo(word(0));
        player.update_step =
            static_cast<int32_t>(kUpdateScale * static_cast<uint32_t>(player.master_tempo) / kTempoScale);
        player.tempo_ticks = 0;
        player.tempo_target = 0;
        player.tempo_step = 0;
        break;

    case 0xE1:
        player.master_volume = Volume8To32(args[0]);
        player.volume_ticks = 0;
        player.volume_target = 0;
        player.volume_step = 0;
        player.volume_changed = true;
        track.changed_volume = true;
        break;

    case 0xE2:
        player.pitch_shift = static_cast<int16_t>(static_cast<int8_t>(args[0]) * 100);
        break;

    case 0xE3:
        effect_changes_[player.bus].type = args[0];
        effect_changes_[player.bus].changed = true;
        break;

    case 0xE4:
        {
            const int32_t time = word(0) == 0 ? 1 : word(0);
            const int32_t tempo = to_tempo(word(2));
            player.tempo_ticks = time;
            player.tempo_target = tempo;
            player.tempo_step = (tempo - player.master_tempo) / time;
            break;
        }

    case 0xE5:
        {
            const int32_t time = word(0) == 0 ? 1 : word(0);
            const int32_t volume = Volume8To32(args[2]);
            player.volume_ticks = time;
            player.volume_target = volume;
            player.volume_step = (volume - player.master_volume) / time;
            break;
        }

    case 0xE6:
        {
            const uint8_t which = args[0];
            if (which < 4 && player.effect_indices[which] >= 0)
            {
                const auto bus = static_cast<std::size_t>(player.effect_indices[which]);
                if (effect_changes_[bus].type != args[1])
                {
                    effect_changes_[bus].type = args[1];
                    effect_changes_[bus].changed = true;
                }

                player.effect_values[which] = args[1];
            }
            break;
        }

    case 0xE8:
        track.patch = args[1];
        track.wave = WaveOf(args[0], track.patch, track.envelope);
        break;

    case 0xE9:
        track.instrument_volume = Volume8To32(args[0]);
        track.changed_volume = true;
        break;

    case 0xEA:
        track.pan = args[0] & 0x7F;
        track.random_pan = 0;
        track.changed_pan = true;
        break;

    case 0xEB:
        track.reverb = args[0] & 0x7F;
        track.changed_reverb = true;
        break;

    case 0xEC:
        track.volume = args[0] & 0x7F;
        track.changed_volume = true;
        break;

    case 0xED:
        track.coarse_detune = static_cast<int16_t>(static_cast<int8_t>(args[0]) * 100);
        break;

    case 0xEE:
        track.fine_detune = static_cast<int8_t>(args[0]);
        break;

    case 0xEF:
        track.detune = static_cast<int16_t>(word(0));
        track.changed_tune = true;
        break;

    case 0xF0:
        track.tremolo_delay = args[0];
        track.tremolo_rate = args[1];
        track.tremolo_depth = args[2];
        break;

    case 0xF1:
        track.tremolo_rate = args[0];
        break;

    case 0xF2:
        track.tremolo_depth = args[0];
        break;

    case 0xF3:
        track.tremolo_depth = 0;
        break;

    case 0xF4:
        track.pan = args[0] & 0x7F;
        track.random_pan = args[1] & 0x7F;
        break;

    case 0xF5:
        {
            BgmInstrument instrument;
            uint32_t which = args[0];
            if (which < 0x80)
            {
                if (which < player.parsed->instruments.size())
                {
                    instrument = player.parsed->instruments[which];
                }
            }
            else
            {
                which -= 0x80;
                if (which < global_instruments_.size())
                {
                    instrument = global_instruments_[which];
                }
            }

            track.patch = static_cast<uint8_t>(instrument.bank_patch);
            track.wave = WaveOf(instrument.bank_patch >> 8, track.patch, track.envelope);
            track.instrument_volume = Volume8To32(instrument.volume);
            track.pan = static_cast<uint8_t>(instrument.pan) & 0x7F;
            track.reverb = instrument.reverb & 0x7F;
            track.coarse_detune = static_cast<int16_t>(instrument.coarse_tune * 100);
            track.fine_detune = instrument.fine_tune;
            track.changed_volume = true;
            track.changed_pan = true;
            track.changed_reverb = true;
            break;
        }

    case 0xF6:
        {
            int32_t time = word(0);
            const int32_t volume = Volume8To32(args[2]);
            if (volume != track.instrument_volume)
            {
                time = time <= 0 ? 1 : time;
                track.volume_ticks = time;
                track.volume_target = volume;
                track.volume_step = (volume - track.instrument_volume) / time;
            }
            break;
        }

    case 0xF7:
        {
            const uint8_t which = args[0];
            track.bus = which < 4 && player.effect_indices[which] >= 0
                            ? static_cast<uint8_t>(player.effect_indices[which])
                            : player.bus;
            break;
        }

    case 0xFC:
        {
            uint32_t table = word(0);
            if (static_cast<uint32_t>(player.proximity_id) < args[2])
            {
                table += static_cast<uint32_t>(player.proximity_id) * 3;
            }

            track.previous_position = track.position;
            track.position = static_cast<uint32_t>((player.file[table] << 8) + player.file[table + 1]);
            track.drums = player.file[table + 2] != 0;
            if (track.proximity_set_changed)
            {
                track.proximity_set_changed = false;
                track.proximity_volume = 0;
                for (uint8_t i = track.first_voice; i < track.last_voice; i++)
                {
                    if (voices_[i].priority == player.priority && voices_[i].has_commands)
                    {
                        ResetVoice(i);
                    }
                }
            }

            track.coarse_detune = 0;
            track.fine_detune = 0;
            track.press_override = 0;
            track.detune = 0;
            track.tremolo_depth = 0;
            track.volume_ticks = 0;
            track.random_pan = 0;
            track.bus = player.bus;
            break;
        }

    case 0xFE:
        track.detour_length = args[2];
        track.saved_position = track.position;
        track.position = word(0);
        break;

    case 0xFF:
        Special(index, track);
        break;

    default:
        break;
    }
}

void SbnPlayer::Special(int index, Track& track)
{
    Player& player = players_[static_cast<std::size_t>(index)];
    const uint8_t type = player.args[0];
    const uint8_t first = player.args[1];
    const uint8_t second = player.args[2];
    switch (type)
    {
    case 1:
        if (first < 4 && player.effect_indices[first] >= 0)
        {
            request_delay_bus_ = static_cast<uint8_t>(player.effect_indices[first]);
            if (second != 0)
            {
                const uint8_t time = second & 0xF;
                const auto side = static_cast<uint8_t>(((second >> 4) & 1) + 1);
                if (request_delay_time_ != time || request_delay_side_ != side)
                {
                    request_delay_time_ = time;
                    request_delay_side_ = side;
                    request_delay_pending_ = true;
                }
            }
            else if (request_delay_side_ != kDelayNone)
            {
                request_delay_side_ = kDelayNone;
                request_delay_pending_ = true;
            }
        }
        break;

    case 2:
        if (first >= 1 && first <= player.custom_envelopes.size())
        {
            player.custom_writing = first;
            player.custom_envelopes[first - 1U].fill(0xFF00);
            player.custom_write[first - 1U] = 0;
        }
        else
        {
            player.custom_writing = 0;
        }
        break;

    case 3:
        {
            const uint8_t writing = player.custom_writing;
            if (writing >= 1 && writing <= player.custom_envelopes.size())
            {
                auto& held = player.custom_envelopes[writing - 1U];
                const uint8_t place = player.custom_write[writing - 1U];
                if (place < held.size() - 1)
                {
                    const uint8_t time = first >= config_.tables.custom_envelope_times.size()
                                             ? first
                                             : config_.tables.custom_envelope_times[first];
                    held[place] = static_cast<uint16_t>((time << 8) + second);
                    player.custom_write[writing - 1U] = static_cast<uint8_t>(place + 1);
                }
            }
            break;
        }

    case 4:
        track.press_override = first <= player.custom_envelopes.size() ? first : uint8_t{0};
        break;

    case 6:
        if (first != 0)
        {
            track.proximity_1 = first;
            track.proximity_2 = second;
        }
        break;

    default:
        break;
    }
}

} // namespace ultralib::n64
