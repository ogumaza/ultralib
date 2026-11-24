// SPDX-License-Identifier: MIT

// The late player of the N64 Music Tools. mus_player.cpp contains the early player.

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "mus_player_common.h"
#include "round_toward_zero.h"
#include "ultralib/n64/mus_player.h"

namespace ultralib::n64
{

namespace
{

using enum mus_player_common::Command;
using mus_player_common::ByteAt;
using mus_player_common::DriverWave;
using mus_player_common::kAttackPhase;
using mus_player_common::kDecayPhase;
using mus_player_common::kDefaultTempoNumerator;
using mus_player_common::kDistortUnitsPerSemitone;
using mus_player_common::kDrumSize;
using mus_player_common::kEndlessLength;
using mus_player_common::kEnvelopeAttack;
using mus_player_common::kEnvelopeDecay;
using mus_player_common::kEnvelopePeak;
using mus_player_common::kEnvelopeRelease;
using mus_player_common::kEnvelopeSize;
using mus_player_common::kEnvelopeSustain;
using mus_player_common::kMostRatio;
using mus_player_common::kNeverOff;
using mus_player_common::kReleasePhase;
using mus_player_common::kRest;
using mus_player_common::kSemitoneExponent;
using mus_player_common::kTempoBeat;
using mus_player_common::kTempoNumerator;
using mus_player_common::kTempoScaleShift;
using mus_player_common::kTempoShift;
using mus_player_common::kTick;
using mus_player_common::kTicksShift;
using mus_player_common::kVibratoUnitsPerSemitone;
using mus_player_common::PowerOfTwo;
using mus_player_common::PowerOfTwoInFloats;
using mus_player_common::ReadBend;
using mus_player_common::ReadVolume;
using mus_player_common::RunCommands;
using mus_player_common::RunCommonCommand;
using mus_player_common::RunNext;
using mus_player_common::Sine;
using mus_player_common::SineInFloats;
using mus_player_common::Sized;
using mus_player_common::Wobble;

} // namespace

// The addresses below are places in the code of Pokémon Puzzle League.

MusLatePlayer::MusLatePlayer(const MusLatePlayerConfig& config, AlSynth& synth)
    : config_(config),
      synth_(&synth),
      frame_microseconds_(1000000 / config.frames_per_second),
      default_tempo_(static_cast<uint16_t>(kDefaultTempoNumerator / config.frames_per_second)),
      random_(config.random_seed)
{
    // The code at 0x8008AF74.
    const int count = config.channels + kMasterChannels;
    channels_.resize(static_cast<std::size_t>(count));
    for (Channel& channel : channels_)
    {
        InitChannel(channel);
    }

    first_voice_ = synth.AddVoices(config.channels);
    for (int i = 0; i < config.channels; i++)
    {
        synth.AllocVoice(first_voice_ + i, config.voice_priority, false);
    }
}

void MusLatePlayer::InitChannel(Channel& channel) const
{
    // The code at 0x8008D77C. A channel's voice goes on sounding.
    const bool playing = channel.playing;
    channel = Channel{};
    channel.playing = playing;
    channel.tempo = default_tempo_;
    channel.base_tempo = default_tempo_;
}

int MusLatePlayer::FindChannel(const MusSong& song, int number) const
{
    // The code at 0x8008D8D4.
    const int count = static_cast<int>(channels_.size());
    const auto channel_at = [&](int i) -> const Channel&
    {
        return channels_[static_cast<std::size_t>(i)];
    };

    if (number < 0)
    {
        for (int i = 0; i < kMasterChannels; i++)
        {
            if (channel_at(i).pc == 0)
            {
                return i;
            }
        }
    }

    for (int i = kMasterChannels; i < count; i++)
    {
        if (channel_at(i).pc == 0)
        {
            return i;
        }
    }

    for (int i = kMasterChannels; i < count; i++)
    {
        if (channel_at(i).song != &song)
        {
            return i;
        }
    }

    if (number >= 0 && static_cast<std::size_t>(number) < song.data_lists.size())
    {
        for (int i = kMasterChannels; i < count; i++)
        {
            if (channel_at(i).data_base == song.data_lists[static_cast<std::size_t>(number)])
            {
                return i;
            }
        }
    }

    return kMasterChannels + (number % (count - kMasterChannels));
}

void MusLatePlayer::StartSong(const MusSong& song, const MusSoundBank& bank)
{
    const RoundTowardZero rounding(config_.round_toward_zero);

    // The code at 0x8008DD3C and 0x8008B2DC.
    const uint32_t handle = next_handle_++;
    const auto start = [&](int number, uint32_t list)
    {
        Channel& channel = channels_[static_cast<std::size_t>(FindChannel(song, number))];
        InitChannel(channel);
        channel.velocity_on = true;
        channel.song = &song;
        channel.bank = bank;
        channel.paused = true;
        channel.handle = handle;
        channel.data_base = list;
        channel.pc = list;

        return &channel;
    };

    start(-1, song.master_list);
    for (std::size_t i = 0; i < song.data_lists.size(); i++)
    {
        if (song.data_lists[i] == 0)
        {
            continue;
        }

        Channel* channel = start(static_cast<int>(i), song.data_lists[i]);
        channel->volume_base = song.volume_lists[i];
        channel->volume_list = song.volume_lists[i];
        channel->bend_base = song.bend_lists[i];
        channel->bend_list = song.bend_lists[i];
    }

    unpause_.push_back(handle);
}

void MusLatePlayer::SetSongMaster(uint16_t volume)
{
    config_.song_master = volume;
}

void MusLatePlayer::SetEffectMaster(uint16_t volume)
{
    config_.effect_master = volume;
}

void MusLatePlayer::UseSynth(AlSynth& synth)
{
    synth_ = &synth;
}

bool MusLatePlayer::Finished() const
{
    return unpause_.empty() && std::ranges::all_of(channels_, [](const Channel& channel) { return channel.pc == 0; });
}

uint32_t MusLatePlayer::RandomSeed() const
{
    return random_;
}

void MusLatePlayer::SetRandomSeed(uint32_t seed)
{
    random_ = seed;
}

void MusLatePlayer::SetEnvelope(Channel& channel, uint32_t place) const
{
    // The code at 0x8008AE20 and 0x8008E594. Unlike the early form, this form uses the first two bytes.
    constexpr int kFullSpeed = 1024;
    uint8_t speed = ByteAt(channel, place);
    speed = speed == 0 ? uint8_t{1} : speed;
    channel.envelope_speed = speed;
    channel.envelope_speed_factor = static_cast<uint32_t>(kFullSpeed / speed);
    channel.envelope_first = ByteAt(channel, place + 1);
    channel.attack = ByteAt(channel, place + kEnvelopeAttack);
    channel.envelope_peak = ByteAt(channel, place + kEnvelopePeak);
    const auto step = [&](float count, int32_t change)
    {
        const auto amount = static_cast<float>(change);
        if (config_.single_precision)
        {
            return (1.0F / count) * amount;
        }

        return static_cast<float>((1.0 / static_cast<double>(count)) * static_cast<double>(amount));
    };

    channel.attack_step =
        step(static_cast<float>(channel.attack), int32_t{channel.envelope_peak} - channel.envelope_first);
    channel.decay = ByteAt(channel, place + kEnvelopeDecay);
    channel.envelope_sustain = ByteAt(channel, place + kEnvelopeSustain);
    channel.decay_step =
        step(static_cast<float>(channel.decay), int32_t{channel.envelope_sustain} - channel.envelope_peak);
    channel.release = ByteAt(channel, place + kEnvelopeRelease);
    channel.release_step = step(static_cast<float>(channel.release), 1);
}

void MusLatePlayer::SetTempo(Channel& channel, uint8_t value)
{
    // The code at 0x8008E6C4.
    const int32_t scaled = (int32_t{value} * kTempoNumerator << kTempoShift) / kTempoBeat;
    const int32_t tempo = scaled / config_.frames_per_second;
    const int32_t applied = (tempo * channel.tempo_scale) >> kTempoScaleShift;
    for (Channel& other : channels_)
    {
        if (other.song == channel.song)
        {
            other.base_tempo = static_cast<uint16_t>(tempo);
            other.tempo = static_cast<uint16_t>(applied);
        }
    }
}

uint32_t MusLatePlayer::RunCommand(Channel& channel, uint8_t command, uint32_t place)
{
    // The table at 0x800BE360.
    constexpr double kTurn = 6.2831852;
    const auto byte = [&](uint32_t at)
    {
        return ByteAt(channel, at);
    };

    // The commands that differ between the two forms. RunCommonCommand runs the others.
    switch (command)
    {
    case kStop:
        channel.volume_list = 0;
        channel.bend_list = 0;
        channel.song = nullptr;
        channel.handle = 0;
        channel.pending_wave = kNoWave;

        return 0;

    case kEnvelopeBytes: // an envelope in the list
        SetEnvelope(channel, place);

        return place + kEnvelopeSize;

    case kTempo:
        SetTempo(channel, byte(place));

        return place + 1;

    case kVibratoUp:
    case kVibratoDown:
        {
            channel.vibrato_delay = byte(place);
            channel.vibrato_speed = byte(place + 1);
            const auto amount = static_cast<float>(byte(place + 2));
            if (config_.single_precision)
            {
                // The code at 0x802F0EE4 and 0x802F0F4C.
                channel.vibrato_depth =
                    (command == kVibratoUp ? amount : -amount) * std::bit_cast<float>(uint32_t{0x3CA3D70A});
                channel.vibrato_step =
                    std::bit_cast<float>(uint32_t{0x40C90FDA}) / static_cast<float>(channel.vibrato_speed);
            }
            else
            {
                channel.vibrato_depth = static_cast<float>(
                    static_cast<double>(command == kVibratoUp ? amount : -amount) / kVibratoUnitsPerSemitone);
                channel.vibrato_step =
                    static_cast<float>(kTurn / static_cast<double>(static_cast<float>(channel.vibrato_speed)));
            }

            return place + 3;
        }

    case kDistort:
        {
            const auto value = static_cast<float>(static_cast<int8_t>(byte(place)));
            channel.distort -= channel.distort_amount;
            channel.distort_amount = config_.single_precision
                                         ? value * std::bit_cast<float>(uint32_t{0x3C23D70A})
                                         : static_cast<float>(static_cast<double>(value) / kDistortUnitsPerSemitone);
            channel.distort += channel.distort_amount;

            return place + 1;
        }

    case kEnvelope: // an envelope of the song
        {
            const uint32_t number = Sized(channel, place);
            SetEnvelope(channel, channel.song->envelopes + number * kEnvelopeSize);

            return place;
        }

    case kEnvelopeOff: // notes keep the note times and the envelope that are running
        channel.keep_note_times = true;

        return place;

    case kEnvelopeOn:
        channel.keep_note_times = false;

        return place;

    case kTriggerOff: // notes keep the wave that is sounding
        channel.keep_wave = true;

        return place;

    case kTriggerOn:
        channel.keep_wave = false;

        return place;

    case kNext:
        return RunNext(channel, place, false);

    case kDrums:
        channel.drums = channel.song->drums + Sized(channel, place) * kDrumSize;

        return place;

    case kDrumsOff:
        channel.drums = 0;

        return place;

    case kSweep:
        channel.sweep = byte(place);

        return place + 1;

    case kChangeEffect: // only a game's setting lets it through
        return place + 1;

    case kMarker: // one byte, then a size
        {
            place++;
            const uint8_t size = byte(place++);

            return (size & 0x80) != 0 ? place + 1 : place;
        }

    case kLengthOff:
        channel.fixed_length = 0;

        return place;

    default:
        return RunCommonCommand(channel, command, place, random_);
    }
}

void MusLatePlayer::StartWave(Channel& channel, int voice)
{
    // The code at 0x8008CA00.
    if (voice < 0)
    {
        channel.pending_wave = kNoWave;

        return;
    }

    const int synth_voice = first_voice_ + voice;
    if (channel.playing)
    {
        synth_->StopVoice(synth_voice);
    }

    channel.playing = true;
    const AlWave out = DriverWave(channel.bank, channel.pending_wave);
    synth_->SetVoiceTag(synth_voice, static_cast<int>(&channel - channels_.data()));
    synth_->StartVoice(synth_voice, out);
    channel.pending_wave = kNoWave;
}

void MusLatePlayer::Step(Channel& channel, int index)
{
    // The code at 0x8008C478. A channel's voice number counts from the first channel after the master channels.
    const int voice = index - kMasterChannels;
    const auto run_command = [&](uint8_t command, uint32_t at)
    {
        return RunCommand(channel, command, at);
    };
    const uint32_t place = RunCommands(channel, run_command);
    channel.pc = place;
    if (place == 0)
    {
        if (channel.playing && voice >= 0)
        {
            channel.playing = false;
            synth_->SetVolume(first_voice_ + voice, 0, frame_microseconds_);
            synth_->StopVoice(first_voice_ + voice);
        }

        return;
    }

    channel.portamento_start = channel.portamento_now;
    const uint32_t note_place = channel.pc;
    uint8_t note = ByteAt(channel, channel.pc++);
    ReadVelocity(channel, note);
    ReadLength(channel);
    channel.note_start = channel.next_event;
    channel.ticks = 0;
    channel.wobble_value = 0;
    channel.wobble_count = channel.wobble_off;
    channel.next_event = channel.note_start + (static_cast<uint64_t>(channel.length) << kTicksShift);
    constexpr uint16_t kNoWaveNumber = 0xFFFF;
    if (channel.song != nullptr && channel.drums == 0 && WaveNumber(channel, channel.wave_number) == kNoWaveNumber)
    {
        note = kRest;
    }

    if (note == kRest)
    {
        ReleaseForRest(channel);

        return;
    }

    if (channel.drums != 0)
    {
        note = StartDrum(channel, note);
    }

    if (!channel.keep_note_times)
    {
        StartNoteTimes(channel);
    }

    if (channel.sweep != 0)
    {
        channel.sweep_time = channel.note_start;
        channel.sweep_step = 0;
        channel.sweep_down = channel.pan & 0x40;
    }

    const int wave = WaveNumber(channel, channel.wave_number);
    const MusBank& bank = *channel.bank.bank;
    if (static_cast<std::size_t>(wave) >= bank.waves.size())
    {
        return;
    }

    if (!channel.keep_wave)
    {
        ChangeWave(channel, voice, wave);
    }

    channel.note_pitch = bank.pitches[static_cast<std::size_t>(wave)] + static_cast<float>(note);
    const int transpose = channel.ignore_transpose ? 0 : int{channel.transpose};
    channel.note_pitch += static_cast<float>(transpose);
    SendReverb(channel, voice);
    if (on_note)
    {
        on_note({index, note, channel.velocity, note_place, wave, channel.note_start});
    }
}

// A rest releases the note that sounds, unless the note is already in its release.
void MusLatePlayer::ReleaseForRest(Channel& channel)
{
    if (channel.envelope_phase < kReleasePhase)
    {
        channel.envelope_phase = kReleasePhase;
        channel.envelope_count = 1;
        channel.note_off = channel.time;
        channel.release_from = channel.envelope_value;
    }
}

// A note's velocity: the byte after the note, or the channel's default.
void MusLatePlayer::ReadVelocity(Channel& channel, uint8_t note) const
{
    if (channel.velocity_on)
    {
        const uint8_t value = ByteAt(channel, channel.pc++);
        const auto velocity = static_cast<uint8_t>(value & 0x7F);
        if (value >= 0x80)
        {
            channel.velocity_on = false;
            channel.default_velocity = velocity;
        }

        if (!config_.rest_keeps_velocity || note != kRest)
        {
            channel.velocity = velocity;
        }
    }
    else
    {
        channel.velocity = channel.default_velocity;
    }
}

// A note's length: the channel's fixed length, or the length after the note.
void MusLatePlayer::ReadLength(Channel& channel) const
{
    if (channel.fixed_length == 0)
    {
        channel.length = static_cast<uint16_t>(Sized(channel, channel.pc));
    }
    else if (!channel.ignore_length)
    {
        channel.length = channel.fixed_length;
    }
    else
    {
        channel.ignore_length = false;
        channel.length = static_cast<uint16_t>(Sized(channel, channel.pc));
    }
}

// The number of a song's wave in the song's table of waves.
uint16_t MusLatePlayer::WaveNumber(const Channel& channel, uint32_t number) const
{
    const uint32_t at = channel.song->wave_table + number * 2;

    return static_cast<uint16_t>((uint32_t{ByteAt(channel, at)} << 8) | ByteAt(channel, at + 1));
}

// A drum's wave, pan and envelope. Returns the drum's note.
uint8_t MusLatePlayer::StartDrum(Channel& channel, uint8_t note) const
{
    const uint32_t drum = channel.drums + uint32_t{note} * kDrumSize;
    channel.wave_number = static_cast<uint16_t>((uint32_t{ByteAt(channel, drum)} << 8) | ByteAt(channel, drum + 1));
    channel.pan = static_cast<uint8_t>(ByteAt(channel, drum + 4) >> 1);
    const uint32_t envelope = (uint32_t{ByteAt(channel, drum + 2)} << 8) | ByteAt(channel, drum + 3);
    SetEnvelope(channel, channel.song->envelopes + envelope * kEnvelopeSize);

    return ByteAt(channel, drum + 5);
}

// The note's end and the start of its envelope.
void MusLatePlayer::StartNoteTimes(Channel& channel)
{
    // The code at 0x8008CD98.
    if (channel.length == kEndlessLength)
    {
        channel.note_off = kNeverOff;
    }
    else if (channel.cutoff != 0)
    {
        channel.note_off = channel.note_start + (static_cast<uint64_t>(channel.cutoff) << kTicksShift);
    }
    else
    {
        channel.note_off = channel.next_event - (static_cast<uint64_t>(channel.end_it) << kTicksShift);
    }

    channel.envelope_count = channel.envelope_speed;
    channel.envelope_phase = kAttackPhase;
    channel.envelope_value = channel.envelope_first;
}

// A new wave for the channel. A voice that still sounds first ramps to silence, and the wave starts after the ramp.
void MusLatePlayer::ChangeWave(Channel& channel, int voice, int wave)
{
    channel.pending_wave = wave;
    if (channel.playing && channel.last_volume != 0 && voice >= 0)
    {
        channel.last_volume = 0;
        synth_->SetVolume(first_voice_ + voice, 0, frame_microseconds_);
    }
    else
    {
        StartWave(channel, voice);
    }
}

// The channel's reverb, when it has changed.
void MusLatePlayer::SendReverb(Channel& channel, int voice)
{
    if (channel.reverb != channel.last_reverb)
    {
        channel.last_reverb = channel.reverb;
        const uint32_t mix = channel.reverb_base + (((128 - uint32_t{channel.reverb_base}) * channel.reverb) >> 7);
        if (voice >= 0)
        {
            synth_->SetFxMix(first_voice_ + voice, static_cast<uint8_t>(mix));
        }
    }
}

// The ticks of an envelope's phase since `from`, less `less`, at the envelope's speed.
int32_t MusLatePlayer::EnvelopeTicks(const Channel& channel, uint64_t from, uint32_t less)
{
    constexpr int kFactorShift = 10;
    const uint32_t ticks = (static_cast<uint32_t>(channel.time - from) >> kTicksShift) - less;

    return static_cast<int32_t>((ticks * channel.envelope_speed_factor) >> kFactorShift);
}

void MusLatePlayer::RunEnvelope(Channel& channel) const
{
    // The code at 0x8008CE10. In Mario Tennis, it's at 0x802F0500, and a note ends once its time has passed.
    constexpr uint8_t kAfterRelease = kReleasePhase + 1;
    const bool ended = config_.end_after_note_off ? channel.time > channel.note_off : channel.time >= channel.note_off;
    if (ended && channel.envelope_phase < kReleasePhase)
    {
        channel.envelope_phase = kReleasePhase;
        channel.envelope_count = 1;
        channel.release_from = channel.envelope_value;
    }

    channel.envelope_count--;
    if (channel.envelope_count != 0)
    {
        return;
    }

    channel.envelope_count = channel.envelope_speed;
    switch (channel.envelope_phase)
    {
    case kAttackPhase:
        {
            const int32_t at = EnvelopeTicks(channel, channel.note_start, 0);
            if (at < channel.attack)
            {
                channel.envelope_value = static_cast<uint8_t>(
                    channel.envelope_first + static_cast<int32_t>(channel.attack_step * static_cast<float>(at)));
            }
            else
            {
                channel.envelope_value = channel.envelope_peak;
                channel.envelope_phase = kDecayPhase;
            }
            break;
        }

    case kDecayPhase:
        {
            const int32_t at = EnvelopeTicks(channel, channel.note_start, channel.attack);
            if (at < channel.decay)
            {
                channel.envelope_value = static_cast<uint8_t>(
                    channel.envelope_peak + static_cast<int32_t>(channel.decay_step * static_cast<float>(at)));
            }
            else
            {
                channel.envelope_value = channel.envelope_sustain;
                channel.envelope_phase = kDecayPhase + 1;
            }
            break;
        }

    case kReleasePhase:
        {
            const int32_t at = EnvelopeTicks(channel, channel.note_off, 0);
            if (at < channel.release)
            {
                const float fall =
                    channel.release_step * static_cast<float>(at) * static_cast<float>(channel.release_from);
                channel.envelope_value = static_cast<uint8_t>(channel.release_from - static_cast<int32_t>(fall));
            }
            else
            {
                channel.envelope_phase = kAfterRelease;
                channel.envelope_value = 0;
            }
            break;
        }

    default:
        break;
    }
}

void MusLatePlayer::RunSweep(Channel& channel)
{
    // The code at 0x8008D044.
    constexpr uint32_t kStepCarry = 64;
    constexpr uint32_t kPanEnd = 128;
    do
    {
        channel.sweep_time += kTick;
        const uint32_t step = uint32_t{channel.sweep_step} + channel.sweep;
        if (step < kStepCarry)
        {
            channel.sweep_step = static_cast<uint8_t>(step);
            continue;
        }

        channel.sweep_step = static_cast<uint8_t>(step & 0x3F);
        const uint32_t carry = step >> 6;
        if (channel.sweep_down == 0)
        {
            channel.pan = static_cast<uint8_t>(channel.pan + carry);
            if (channel.pan >= kPanEnd)
            {
                channel.pan = kPanEnd - 1;
                channel.sweep_down = 1;
            }
        }
        else
        {
            channel.pan = static_cast<uint8_t>(channel.pan - carry);
            if (channel.pan >= kPanEnd || channel.pan == 0)
            {
                channel.pan = 0;
                channel.sweep_down = 0;
            }
        }
    } while (channel.sweep_time < channel.time);
}

float MusLatePlayer::Vibrato(Channel& channel) const
{
    // The code at 0x8008D158.
    const int32_t count = int32_t{channel.ticks} - channel.vibrato_delay;
    if (count <= 0)
    {
        return 0.0F;
    }

    float angle = channel.vibrato_step * static_cast<float>(count);
    if (config_.single_precision)
    {
        // The code at 0x802F07C8.
        if (!(std::abs(angle) <= std::bit_cast<float>(uint32_t{0x40490FDB})))
        {
            const float turns = std::nearbyintf(angle * std::bit_cast<float>(uint32_t{0x3E22F983}));
            angle -= turns * std::bit_cast<float>(uint32_t{0x40C90FDB});
        }

        channel.vibrato = SineInFloats(angle) * channel.vibrato_depth;
    }
    else
    {
        channel.vibrato = Sine(angle) * channel.vibrato_depth;
    }

    return channel.vibrato;
}

void MusLatePlayer::SendVolumeAndPan(Channel& channel, int voice)
{
    // The code at 0x8008CA90.
    constexpr uint32_t kTop = 32767;
    constexpr int kFirstShift = 13;
    constexpr int kMasterShift = 15;
    if (!config_.stereo)
    {
        channel.pan_scale = 128;
        channel.pan = 64;
    }

    uint32_t volume = 0;
    if (channel.velocity_scale != 0)
    {
        volume = uint32_t{channel.volume} * channel.envelope_value * channel.velocity *
                 static_cast<uint32_t>(static_cast<int32_t>(channel.velocity_scale));
        volume >>= kFirstShift;
        volume = std::min(volume, kTop);
        volume = (volume * config_.song_master) >> kMasterShift;
    }

    if (volume != channel.last_volume)
    {
        channel.last_volume = static_cast<uint16_t>(volume);
        if (voice >= 0)
        {
            synth_->SetVolume(first_voice_ + voice, static_cast<int16_t>(volume), frame_microseconds_);
        }
    }

    if (channel.pan != channel.last_pan)
    {
        const auto pan = static_cast<uint8_t>(((int32_t{channel.pan} * channel.pan_scale) >> 7) & 0x7F);
        channel.last_pan = pan;
        if (voice >= 0)
        {
            synth_->SetPan(first_voice_ + voice, pan);
        }
    }
}

void MusLatePlayer::SendPitch(Channel& channel, int voice, float offset)
{
    // The code at 0x8008CC78.
    float pitch = channel.note_pitch;
    if (channel.portamento != 0)
    {
        if (channel.portamento >= channel.ticks)
        {
            const float slope = (pitch - channel.portamento_start) / static_cast<float>(channel.portamento);
            pitch = channel.portamento_start + slope * static_cast<float>(channel.ticks);
        }

        channel.portamento_now = pitch;
    }

    pitch += offset + channel.bend;
    if (pitch == channel.last_pitch)
    {
        return;
    }

    channel.last_pitch = pitch;
    float ratio = config_.single_precision
                      ? PowerOfTwoInFloats(pitch * std::bit_cast<float>(uint32_t{0x3DAAAAAB}))
                      : PowerOfTwo(static_cast<float>(static_cast<double>(pitch) * kSemitoneExponent));
    if (static_cast<double>(ratio) > kMostRatio)
    {
        ratio = static_cast<float>(kMostRatio);
        channel.velocity = 0;
    }

    if (voice >= 0)
    {
        synth_->SetPitch(first_voice_ + voice, ratio);
    }
}

void MusLatePlayer::RunChannel(Channel& channel, int index)
{
    // The code at 0x8008C238.
    const int voice = index - kMasterChannels;
    if (channel.pc == 0 || channel.paused)
    {
        return;
    }

    if (channel.pending_wave != kNoWave)
    {
        StartWave(channel, voice);
    }

    channel.time += channel.tempo;
    if (channel.length != kEndlessLength)
    {
        while (channel.next_event < channel.time)
        {
            Step(channel, index);
            if (channel.pc == 0)
            {
                return;
            }
        }
    }

    if (channel.volume_list != 0 && channel.volume_time < channel.time)
    {
        ReadVolume(channel);
    }

    if (channel.bend_list != 0 && channel.bend_time < channel.time)
    {
        ReadBend(channel);
    }

    if (channel.playing)
    {
        if (channel.envelope_phase != 0)
        {
            RunEnvelope(channel);
        }

        if (channel.sweep != 0 && channel.sweep_time < channel.time)
        {
            RunSweep(channel);
        }

        float offset = channel.distort;
        if (channel.vibrato_speed != 0)
        {
            offset += Vibrato(channel);
        }

        if (channel.wobble_on != 0)
        {
            offset += Wobble(channel);
        }

        if (channel.pending_wave == kNoWave)
        {
            if (config_.pitch_before_volume)
            {
                SendPitch(channel, voice, offset);
                SendVolumeAndPan(channel, voice);
            }
            else
            {
                SendVolumeAndPan(channel, voice);
                SendPitch(channel, voice, offset);
            }
        }
    }

    channel.ticks = static_cast<uint16_t>(static_cast<uint32_t>(channel.time - channel.note_start) >> kTicksShift);
}

int32_t MusLatePlayer::RunFrame()
{
    const RoundTowardZero rounding(config_.round_toward_zero);

    // The code at 0x8008C170.
    for (const uint32_t handle : unpause_)
    {
        for (Channel& channel : channels_)
        {
            if (channel.handle == handle)
            {
                channel.paused = false;
            }
        }
    }

    unpause_.clear();
    for (std::size_t i = 0; i < channels_.size(); i++)
    {
        RunChannel(channels_[i], static_cast<int>(i));
    }

    return frame_microseconds_;
}

} // namespace ultralib::n64
