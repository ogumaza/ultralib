// SPDX-License-Identifier: MIT

// The addresses in the comments are places in the code of Pokémon Stadium, the first game that this model was checked
// against.

#include "ultralib/n64/mus_player.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "mus_player_common.h"
#include "round_toward_zero.h"

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
using mus_player_common::ReadBend;
using mus_player_common::ReadVolume;
using mus_player_common::RunCommands;
using mus_player_common::RunCommonCommand;
using mus_player_common::RunNext;
using mus_player_common::Sine;
using mus_player_common::Sized;
using mus_player_common::Wobble;

constexpr int kFreePhase = 5;
constexpr int kDefaultChannelPriority = 60;
constexpr int kVoicePriorityLimit = 127;

constexpr double kPi = 3.1415926;
constexpr float kFirstDecayStep = 0.003921568859368563F;

} // namespace

MusPlayer::MusPlayer(const MusPlayerConfig& config, AlSynth& synth)
    : config_(config),
      synth_(&synth),
      frame_microseconds_(1000000 / config.frames_per_second),
      default_tempo_(static_cast<uint16_t>(kDefaultTempoNumerator / config.frames_per_second)),
      random_(config.random_seed)
{
    channels_.resize(static_cast<std::size_t>(config.channels));
    for (Channel& channel : channels_)
    {
        InitChannel(channel);
    }

    const int first = synth.AddVoices(config.voices);
    voices_.resize(static_cast<std::size_t>(config.voices));
    for (int i = 0; i < config.voices; i++)
    {
        voices_[static_cast<std::size_t>(i)].phase = kFreePhase;
        voices_[static_cast<std::size_t>(i)].synth_voice = first + i;
        free_voices_.push_back(i);
    }
}

void MusPlayer::InitChannel(Channel& channel) const
{
    channel = Channel{};
    channel.tempo = default_tempo_;
    channel.base_tempo = default_tempo_;
    channel.decay_step = kFirstDecayStep;
}

void MusPlayer::StartSong(const MusSong& song, const MusSoundBank& bank)
{
    pending_.push_back({&song, bank});
}

void MusPlayer::SetSongMaster(uint16_t volume)
{
    config_.song_master = volume;
}

void MusPlayer::SetEffectMaster(uint16_t volume)
{
    config_.effect_master = volume;
}

uint32_t MusPlayer::RandomSeed() const
{
    return random_;
}

void MusPlayer::SetRandomSeed(uint32_t seed)
{
    random_ = seed;
}

void MusPlayer::UseSynth(AlSynth& synth)
{
    synth_ = &synth;
}

bool MusPlayer::Finished() const
{
    return release_voices_.empty() && pending_.empty() &&
           std::ranges::all_of(channels_,
                               [](const Channel& channel) { return channel.pc == 0 && channel.voice == kNoVoice; });
}

void MusPlayer::StartPending(const PendingSong& pending)
{
    // The code at 0x80039E98. Each channel of the song takes the first free channel of the player.
    const MusSong& song = *pending.song;
    for (std::size_t i = 0; i < song.data_lists.size(); i++)
    {
        if (song.data_lists[i] == 0)
        {
            continue;
        }

        const auto found = std::ranges::find_if(channels_, [](const Channel& channel) { return channel.pc == 0; });
        if (found == channels_.end())
        {
            continue;
        }

        Channel& channel = *found;
        InitChannel(channel);
        channel.song = pending.song;
        channel.bank = pending.bank;
        channel.volume_base = song.volume_lists[i];
        channel.volume_list = song.volume_lists[i];
        channel.bend_base = song.bend_lists[i];
        channel.bend_list = song.bend_lists[i];
        channel.data_base = song.data_lists[i];
        channel.pc = song.data_lists[i];
        channel.priority = kDefaultChannelPriority;
    }
}

void MusPlayer::SetEnvelope(Channel& channel, uint32_t place)
{
    // The code at 0x80038178. The early form takes the first byte as the speed. Nothing reads the speed. The form
    // ignores the second byte.
    channel.envelope_first = 0;
    uint8_t attack = ByteAt(channel, place + kEnvelopeAttack);
    attack = attack == 0 ? uint8_t{1} : attack;
    channel.attack = attack;
    channel.envelope_peak = ByteAt(channel, place + kEnvelopePeak);
    channel.attack_step = static_cast<float>(
        (1.0 / static_cast<double>(static_cast<float>(attack))) *
        static_cast<double>(static_cast<float>(int32_t{channel.envelope_peak} - channel.envelope_first)));
    uint8_t decay = ByteAt(channel, place + kEnvelopeDecay);
    decay = decay == 0 ? uint8_t{1} : decay;
    channel.decay = decay;
    channel.envelope_sustain = ByteAt(channel, place + kEnvelopeSustain);
    channel.decay_step = (1.0F / static_cast<float>(decay)) *
                         (static_cast<float>(channel.envelope_sustain) - static_cast<float>(channel.envelope_peak));
    const uint8_t release = ByteAt(channel, place + kEnvelopeRelease);
    channel.release = release == 0 ? uint8_t{1} : release;
}

void MusPlayer::SetTempo(Channel& channel, uint8_t value)
{
    // The code at 0x800382F4. A tempo reaches each channel of the song. A sound effect's channel keeps its tempo apart.
    const int32_t scaled = (int32_t{value} * kTempoNumerator << kTempoShift) / kTempoBeat;
    const auto tempo = static_cast<uint32_t>(scaled) / static_cast<uint32_t>(config_.frames_per_second);
    const uint32_t applied =
        (static_cast<uint32_t>(static_cast<int32_t>(channel.tempo_scale)) * tempo) >> kTempoScaleShift;
    if (channel.handle != 0)
    {
        channel.tempo = static_cast<uint16_t>(tempo);

        return;
    }

    for (Channel& other : channels_)
    {
        if (other.song == channel.song)
        {
            other.base_tempo = static_cast<uint16_t>(tempo);
            other.tempo = static_cast<uint16_t>(applied);
        }
    }
}

uint32_t MusPlayer::RunCommand(Channel& channel, uint8_t command, uint32_t place)
{
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
            channel.vibrato_depth = static_cast<float>(static_cast<double>(command == kVibratoUp ? amount : -amount) /
                                                       kVibratoUnitsPerSemitone);

            return place + 3;
        }

    case kDistort:
        {
            const auto value = static_cast<float>(static_cast<int8_t>(byte(place)));
            channel.distort -= channel.distort_amount;
            channel.distort_amount = static_cast<float>(static_cast<double>(value) / kDistortUnitsPerSemitone);
            channel.distort += channel.distort_amount;

            return place + 1;
        }

    case kEnvelope: // an envelope of the song
        {
            const uint32_t number = Sized(channel, place);
            SetEnvelope(channel, channel.song->envelopes + number * kEnvelopeSize);

            return place;
        }

    case kEnvelopeOff: // the code at 0x800385EC sets a flag that nothing reads
    case kEnvelopeOn:
    case kTriggerOff:
    case kTriggerOn:
        return place;

    case kNext:
        return RunNext(channel, place, true);

    case kDrums: // nothing in the early form reads them
        return place + 1;

    case kDrumsOff:
        return place;

    case kSweep:
        channel.sweep = byte(place);
        if (channel.sweep == 0)
        {
            channel.pan_scale = 128;
        }

        return place + 1;

    default:
        return RunCommonCommand(channel, command, place, random_);
    }
}

void MusPlayer::NoteTimes(Channel& channel) const
{
    // The code at 0x8003B354.
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

    channel.envelope_phase = kAttackPhase;
    channel.envelope_value = channel.envelope_first;
}

void MusPlayer::Step(Channel& channel, int index)
{
    // The code at 0x8003AE98.
    const auto run_command = [&](uint8_t command, uint32_t at)
    {
        return RunCommand(channel, command, at);
    };
    const uint32_t place = RunCommands(channel, run_command);
    channel.pc = place;
    if (place == 0)
    {
        EndNote(channel);

        return;
    }

    channel.portamento_start = channel.portamento_now;
    const uint32_t note_place = channel.pc;
    const uint8_t note = ByteAt(channel, channel.pc++);
    channel.velocity = channel.velocity_on ? ByteAt(channel, channel.pc++) : channel.default_velocity;
    if (channel.fixed_length != 0 && !channel.ignore_length)
    {
        channel.length = channel.fixed_length;
    }
    else
    {
        channel.ignore_length = false;
        channel.length = static_cast<uint16_t>(Sized(channel, channel.pc));
    }

    channel.note_start = channel.next_event;
    channel.next_event = channel.note_start + (static_cast<uint64_t>(channel.length) << kTicksShift);
    channel.ticks = 0;
    channel.wobble_value = 0;
    channel.wobble_count = channel.wobble_off;
    if (note == kRest)
    {
        EndNote(channel);

        return;
    }

    NoteTimes(channel);
    if (channel.sweep != 0)
    {
        channel.sweep_step = 0;
        channel.sweep_down = static_cast<uint8_t>(channel.pan_scale & 0x80);
        channel.sweep_time = static_cast<uint32_t>(channel.note_start);
    }

    if (channel.playing)
    {
        channel.wave = kNoWave;
        RunEnvelope(channel);
    }

    const std::size_t wave = channel.wave_number;
    const MusBank& bank = *channel.bank.bank;
    if (wave >= bank.waves.size())
    {
        channel.wave = kNoWave;

        return;
    }

    channel.wave = static_cast<int>(wave);
    channel.note_pitch = bank.pitches[wave] + static_cast<float>(note);
    if (on_note)
    {
        on_note({index, note, channel.velocity, note_place, channel.wave, channel.note_start});
    }
}

// Ends the channel's note. A voice that still plays the note starts its release.
void MusPlayer::EndNote(Channel& channel)
{
    channel.wave = kNoWave;
    if (channel.playing)
    {
        channel.note_off = channel.time;
        RunEnvelope(channel);
    }
}

void MusPlayer::ReleaseChannelVoice(Channel& channel)
{
    channel.voice = kNoVoice;
    channel.playing = false;
    channel.wave = kNoWave;
}

void MusPlayer::RunEnvelope(Channel& channel)
{
    // The code at 0x8003B3F4.
    if (channel.voice == kNoVoice)
    {
        const auto ticks = static_cast<uint32_t>(channel.time - channel.note_start) >> kTicksShift;
        if (ticks < channel.attack && channel.attack >= 2)
        {
            channel.envelope_phase = kAttackPhase;
            channel.envelope_value =
                static_cast<uint8_t>(static_cast<uint32_t>(channel.attack_step * static_cast<float>(ticks)));
        }
        else
        {
            channel.envelope_phase = kDecayPhase;
            channel.envelope_value = channel.envelope_peak;
        }

        return;
    }

    VoiceState& voice = voices_[static_cast<std::size_t>(channel.voice)];
    if ((channel.time >= channel.note_off && voice.phase < kReleasePhase) || channel.wave == kNoWave)
    {
        voice.phase = kReleasePhase;
        constexpr double kTempoUnit = 0.00390625;
        const double frames = static_cast<double>(static_cast<float>(voice.release)) /
                              (static_cast<double>(static_cast<float>(channel.tempo)) * kTempoUnit);
        uint32_t count = static_cast<uint32_t>(frames);
        count = count == 0 ? 1 : count;
        voice.end_frame = frame_ + count + 1;
        synth_->SetVolume(voice.synth_voice, 0,
                          static_cast<int32_t>(static_cast<uint32_t>(frame_microseconds_) * count));
        if (channel.priority < 128)
        {
            synth_->SetPriority(voice.synth_voice, channel.handle != 0 ? 1 : 0);
        }

        release_voices_.push_back(channel.voice);
        ReleaseChannelVoice(channel);

        return;
    }

    if (voice.phase == kAttackPhase)
    {
        const auto ticks = static_cast<uint32_t>(channel.time - channel.note_start) >> kTicksShift;
        if (ticks < channel.attack)
        {
            channel.envelope_value =
                static_cast<uint8_t>(static_cast<uint32_t>(channel.attack_step * static_cast<float>(ticks)));
        }
        else
        {
            voice.phase = kDecayPhase;
            channel.envelope_value = channel.envelope_peak;
        }
    }
    else if (voice.phase == kDecayPhase)
    {
        const auto ticks = static_cast<uint32_t>((channel.time - channel.note_start) >> kTicksShift);
        const uint32_t since = ticks - channel.attack;
        if (since < channel.decay)
        {
            channel.envelope_value = static_cast<uint8_t>(
                channel.envelope_peak + static_cast<int32_t>(channel.decay_step * static_cast<float>(since)));
        }
        else
        {
            voice.phase++;
            channel.envelope_value = channel.envelope_sustain;
        }
    }
}

void MusPlayer::RunSweep(Channel& channel) const
{
    // The code at 0x8003B8C8.
    constexpr uint32_t kStepCarry = 128;
    constexpr uint32_t kTopPan = 255;
    do
    {
        channel.sweep_time += kTick;
        const uint32_t step = uint32_t{channel.sweep_step} + (uint32_t{channel.sweep} << 1);
        if (step < kStepCarry)
        {
            channel.sweep_step = static_cast<uint8_t>(step);
        }
        else
        {
            channel.sweep_step = static_cast<uint8_t>(step & 0x7F);
            const uint32_t carry = step >> 7;
            if (channel.sweep_down == 0)
            {
                uint32_t pan = static_cast<uint32_t>(static_cast<int32_t>(channel.pan_scale)) + carry;
                if (pan >= kTopPan)
                {
                    pan = kTopPan - 1;
                    channel.sweep_down = 1;
                }

                channel.pan_scale = static_cast<int16_t>(pan);
            }
            else
            {
                uint32_t pan = static_cast<uint32_t>(static_cast<int32_t>(channel.pan_scale)) - carry;
                if (pan >= kTopPan || pan == 0)
                {
                    pan = 1;
                    channel.sweep_down = 0;
                }

                channel.pan_scale = static_cast<int16_t>(pan);
            }
        }
    } while (channel.sweep_time < channel.time);

    channel.last_pan = 255;
}

float MusPlayer::Vibrato(Channel& channel) const
{
    // The code at 0x8003B9E4. The count wraps while the delay runs, to near 2^32. For a speed up to 100, the sine's
    // argument is then 2^28 or more, and the game's sine returns 0 up to the delay's end.
    const uint32_t count = uint32_t{channel.ticks} - channel.vibrato_delay;
    if (count != 0)
    {
        constexpr float kTurn = 2.0F;
        const float turns = static_cast<float>(count) / static_cast<float>(channel.vibrato_speed) * kTurn;
        channel.vibrato = Sine(static_cast<float>(static_cast<double>(turns) * kPi)) * channel.vibrato_depth;
    }

    return channel.vibrato;
}

float MusPlayer::Pitch(Channel& channel, float offset) const
{
    // The code at 0x8003B21C. It returns 0 when the pitch hasn't changed.
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

    const int transpose = int{channel.transpose} * (channel.ignore_transpose ? 0 : 1);
    channel.ignore_transpose = false;
    pitch += (offset + channel.bend) + static_cast<float>(transpose);
    if (pitch == channel.last_pitch)
    {
        return 0.0F;
    }

    channel.last_pitch = pitch;
    const float ratio = PowerOfTwo(static_cast<float>(static_cast<double>(pitch) * kSemitoneExponent));
    if (static_cast<double>(ratio) > kMostRatio)
    {
        channel.velocity = 0;

        return static_cast<float>(kMostRatio);
    }

    return ratio;
}

uint32_t MusPlayer::VoiceVolume(const Channel& channel) const
{
    // The code at 0x8003B14C.
    constexpr uint32_t kTop = 0x8000;
    constexpr int kFirstShift = 13;
    constexpr int kMasterShift = 15;
    uint32_t volume = uint32_t{channel.volume} * channel.envelope_value * channel.velocity *
                      static_cast<uint32_t>(static_cast<int32_t>(channel.velocity_scale));
    volume >>= kFirstShift;
    volume = (volume * (channel.handle == 0 ? config_.song_master : config_.effect_master)) >> kMasterShift;
    if (volume >= kTop)
    {
        volume = kTop - 1;
    }

    if (channel.fade != -1)
    {
        volume = static_cast<uint32_t>(channel.fade) * volume / static_cast<uint32_t>(channel.fade_total);
    }

    return volume;
}

void MusPlayer::RunChannel(Channel& channel, int index)
{
    if (channel.pc == 0)
    {
        return;
    }

    // The code at 0x8003A944.
    channel.time += channel.tempo;
    if (channel.length != kEndlessLength)
    {
        while (channel.pc != 0 && channel.time > channel.next_event)
        {
            Step(channel, index);
        }
    }

    if (channel.volume_list != 0 && channel.time > channel.volume_time)
    {
        ReadVolume(channel);
    }

    if (channel.bend_list != 0 && channel.time > channel.bend_time)
    {
        ReadBend(channel);
    }

    if (channel.fade != -1)
    {
        channel.fade--;
        if (channel.fade == -1)
        {
            EndFade(channel);
        }
    }

    if (channel.wave == kNoWave)
    {
        return;
    }

    RunEnvelope(channel);
    if (channel.wave == kNoWave)
    {
        return;
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

    const float ratio = Pitch(channel, offset);
    const uint32_t volume = VoiceVolume(channel);
    if (channel.sweep != 0 && channel.time > channel.sweep_time)
    {
        RunSweep(channel);
    }

    const uint32_t pan =
        config_.mono
            ? 64
            : ((uint32_t{channel.pan} * static_cast<uint32_t>(static_cast<int32_t>(channel.pan_scale))) >> 7) & 0x7F;
    const uint32_t reverb = channel.reverb_base + (((128 - uint32_t{channel.reverb_base}) * channel.reverb) >> 7);
    channel.ticks = static_cast<uint16_t>(static_cast<uint32_t>(channel.time - channel.note_start) >> kTicksShift);
    if (channel.playing)
    {
        SendVoice(channel, ratio, volume, pan, reverb);

        return;
    }

    StartVoice(channel, ratio, volume, pan, reverb);
}

// The end of a channel's fade: the channel stops, and its voice is free again.
void MusPlayer::EndFade(Channel& channel)
{
    channel.pc = RunCommand(channel, 0x80, 0);
    if (channel.playing)
    {
        VoiceState& voice = voices_[static_cast<std::size_t>(channel.voice)];
        channel.playing = false;
        synth_->StopVoice(voice.synth_voice);
        synth_->FreeVoice(voice.synth_voice);
        voice.phase = kFreePhase;
        free_voices_.push_back(channel.voice);
        channel.voice = kNoVoice;
        channel.handle = 0;
    }

    channel.wave = kNoWave;
}

// The changes of a playing voice's pitch, volume, pan and reverb.
void MusPlayer::SendVoice(Channel& channel, float ratio, uint32_t volume, uint32_t pan, uint32_t reverb)
{
    const int synth_voice = voices_[static_cast<std::size_t>(channel.voice)].synth_voice;
    if (ratio != 0.0F)
    {
        synth_->SetPitch(synth_voice, ratio);
    }

    if (volume != channel.last_volume)
    {
        channel.last_volume = static_cast<uint16_t>(volume);
        synth_->SetVolume(synth_voice, static_cast<int16_t>(volume), frame_microseconds_);
    }

    if (pan != channel.last_pan)
    {
        channel.last_pan = static_cast<uint8_t>(pan);
        synth_->SetPan(synth_voice, static_cast<uint8_t>(pan));
    }

    if (channel.last_reverb != channel.reverb)
    {
        channel.last_reverb = static_cast<uint8_t>(reverb);
        synth_->SetFxMix(synth_voice, static_cast<uint8_t>(reverb));
    }
}

// Starts a voice for the channel's note, when a voice is free.
void MusPlayer::StartVoice(Channel& channel, float ratio, uint32_t volume, uint32_t pan, uint32_t reverb)
{
    if (free_voices_.empty())
    {
        channel.wave = kNoWave;

        return;
    }

    channel.voice = free_voices_.front();
    free_voices_.pop_front();
    VoiceState& voice = voices_[static_cast<std::size_t>(channel.voice)];
    voice.phase = channel.envelope_phase;
    voice.release = channel.release;
    synth_->AllocVoice(voice.synth_voice, static_cast<int16_t>(std::min(channel.priority, kVoicePriorityLimit)), false);
    channel.last_volume = static_cast<uint16_t>(volume);
    channel.last_pan = static_cast<uint8_t>(pan);
    channel.last_reverb = static_cast<uint8_t>(reverb);
    if (ratio == 0.0F)
    {
        ratio = PowerOfTwo(static_cast<float>(static_cast<double>(channel.last_pitch) * kSemitoneExponent));
        if (static_cast<double>(ratio) > kMostRatio)
        {
            ratio = static_cast<float>(kMostRatio);
        }
    }

    synth_->SetVoiceTag(voice.synth_voice, static_cast<int>(&channel - channels_.data()));
    synth_->StartVoiceParams(voice.synth_voice, DriverWave(channel.bank, channel.wave), ratio,
                             static_cast<int16_t>(volume), static_cast<uint8_t>(pan), static_cast<uint8_t>(reverb),
                             config_.ramp_new_voices ? frame_microseconds_ : 1);
    channel.playing = true;
    voice.phase = kAttackPhase;
}

void MusPlayer::ExpireVoices()
{
    // The code at 0x8003C548.
    for (std::size_t i = 0; i < release_voices_.size();)
    {
        const int number = release_voices_[i];
        VoiceState& voice = voices_[static_cast<std::size_t>(number)];
        if (voice.end_frame >= frame_)
        {
            i++;
            continue;
        }

        synth_->StopVoice(voice.synth_voice);
        synth_->FreeVoice(voice.synth_voice);
        voice.phase = kFreePhase;
        release_voices_.erase(release_voices_.begin() + static_cast<std::ptrdiff_t>(i));
        free_voices_.push_back(number);
    }
}

void MusPlayer::StopAll()
{
    // The code at 0x80039A80.
    constexpr uint32_t kReleaseFrames = 2;
    for (Channel& channel : channels_)
    {
        if (channel.handle != 0 || channel.pc == 0)
        {
            continue;
        }

        channel.pc = RunCommand(channel, 0x80, 0);
        if (!channel.playing)
        {
            channel.wave = kNoWave;
            continue;
        }

        VoiceState& voice = voices_[static_cast<std::size_t>(channel.voice)];
        voice.phase = kReleasePhase;
        voice.end_frame = frame_ + kReleaseFrames;
        synth_->SetVolume(voice.synth_voice, 0, static_cast<int32_t>(frame_microseconds_ * kReleaseFrames));
        synth_->SetPriority(voice.synth_voice, 0);
        release_voices_.push_back(channel.voice);
        ReleaseChannelVoice(channel);
    }
}

int32_t MusPlayer::RunFrame()
{
    const RoundTowardZero rounding(config_.round_toward_zero);
    for (const PendingSong& pending : pending_)
    {
        StartPending(pending);
    }

    pending_.clear();
    for (std::size_t i = 0; i < channels_.size(); i++)
    {
        RunChannel(channels_[i], static_cast<int>(i));
    }

    ExpireVoices();
    frame_++;

    return frame_microseconds_;
}

} // namespace ultralib::n64
