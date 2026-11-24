// SPDX-License-Identifier: MIT

// The voices of libultra's sequence players: their sounds in the bank, their levels and pans, and the state of each
// channel.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "al_player_constants.h"
#include "ultralib/n64/al_player.h"
#include "ultralib/n64/al_synth.h"

namespace ultralib::n64
{

namespace
{

using al_player_constants::kVolumeFull;
using al_player_constants::OffsetFxMix;

constexpr int32_t kGainChangeTime = 1000;
constexpr int32_t kInitialTickTime = 488;
constexpr uint8_t kPanCenter = 64;
constexpr uint8_t kScaledFormVolume = 120;
constexpr uint8_t kDefaultPriority = 5;

// AlPlayerForm::kPlayerScale's rate of a decay, for each 8 microseconds. The table holds the base-2 logarithm of each
// value of a number's first four bits.
float DecayRate(float start, float target, int32_t time)
{
    static constexpr std::array<double, 8> kLogarithms = {-0.912537, -0.752072, -0.607683, -0.476438,
                                                          -0.356144, -0.245112, -0.142019, -0.045804};
    static constexpr double kLogOfTwo = 0.6931471805599453;
    if (time == 0)
    {
        return start <= target ? 32768.0F : 0.0F;
    }

    const double from = std::max(static_cast<double>(start), 1.0);
    const double to = std::max(static_cast<double>(target), 1.0);
    auto bits = static_cast<int32_t>(std::ldexp(1.0 / static_cast<double>(time), 30));
    int exponent = 0;
    const double mantissa = std::frexp(to / from, &exponent);
    const auto index = static_cast<std::size_t>(std::ldexp(mantissa, 4));
    const double sum = kLogarithms[index - 8] + static_cast<double>(exponent);
    const double logarithm = sum * kLogOfTwo;
    const double step = logarithm / std::ldexp(1.0, 30);
    double base = 1.0 + step;
    double power = 1.0;
    while (bits != 0)
    {
        if ((bits & 1) != 0)
        {
            power *= base;
        }

        base *= base;
        bits >>= 1;
    }

    const double squared = power * power;
    const double fourth = squared * squared;

    return static_cast<float>(fourth * fourth);
}

// The gain of a decay after `time` microseconds.
float DecayGain(float start, int32_t time, float rate)
{
    int32_t steps = time >> 3;
    if (steps == 0)
    {
        return start;
    }

    float power = 1.0F;
    for (int i = 0; i < 32; i++)
    {
        if ((steps & 1) != 0)
        {
            power *= rate;
        }

        steps >>= 1;
        if (steps == 0)
        {
            break;
        }

        rate *= rate;
    }

    return start * power;
}

} // namespace

void AlPlayer::ReleaseVoice(AlSynth& synth, int voice_index, int32_t time)
{
    VoiceState& voice = voices_[static_cast<std::size_t>(voice_index)];
    if (voice.envelope_phase == EnvelopePhase::kAttack)
    {
        RemoveEnvelopeEvents(voice_index);
    }

    voice.velocity = 0;
    voice.envelope_phase = EnvelopePhase::kRelease;
    voice.envelope_gain = 0;
    voice.envelope_end = time_ + time;
    synth.SetPriority(voice.synth_voice, 0);
    synth.SetVolume(voice.synth_voice, 0, time);

    Event event;
    event.type = EventType::kNoteEnd;
    event.voice = voice_index;
    Post(event, time);
}

int AlPlayer::MapVoice(uint8_t key, uint8_t velocity, uint8_t channel)
{
    if (free_voices_.empty())
    {
        return -1;
    }

    const int index = free_voices_.back();
    free_voices_.pop_back();
    active_voices_.push_back(index);
    VoiceState& voice = voices_[static_cast<std::size_t>(index)];
    voice.channel = channel;
    voice.key = key;
    voice.velocity = velocity;

    return index;
}

void AlPlayer::UnmapVoice(int voice)
{
    const auto place = std::find(active_voices_.begin(), active_voices_.end(), voice);
    if (place != active_voices_.end())
    {
        active_voices_.erase(place);
        free_voices_.push_back(voice);
    }
}

int AlPlayer::LookupVoice(uint8_t key, uint8_t channel) const
{
    for (const int index : active_voices_)
    {
        const VoiceState& voice = voices_[static_cast<std::size_t>(index)];
        if (voice.key == key && voice.channel == channel && voice.phase != NotePhase::kRelease &&
            voice.phase != NotePhase::kSustainRelease)
        {
            return index;
        }
    }

    return -1;
}

const BankSound* AlPlayer::LookupSound(uint8_t key, uint8_t velocity, uint8_t channel) const
{
    const BankInstrument* instrument = channels_state_[channel].instrument;
    if (instrument == nullptr)
    {
        return nullptr;
    }

    // A binary search. The library counts from 1.
    int left = 1;
    int right = static_cast<int>(instrument->sounds.size());
    while (right >= left)
    {
        const int middle = (left + right) / 2;
        const BankSound& sound =
            file_->sounds[static_cast<std::size_t>(instrument->sounds[static_cast<std::size_t>(middle - 1)])];
        const BankKeyMap& key_map = file_->key_maps[static_cast<std::size_t>(sound.key_map)];
        if (key >= key_map.key_min && key <= key_map.key_max && velocity >= key_map.velocity_min &&
            velocity <= key_map.velocity_max)
        {
            return &sound;
        }

        if (key < key_map.key_min || (velocity < key_map.velocity_min && key <= key_map.key_max))
        {
            right = middle - 1;
        }
        else
        {
            left = middle + 1;
        }
    }

    return nullptr;
}

int16_t AlPlayer::VoiceVolume(const VoiceState& voice) const
{
    const ChannelState& state = channels_state_[voice.channel];
    auto note = static_cast<uint32_t>((voice.tremolo * voice.velocity * voice.envelope_gain) >> 6);
    auto channel = static_cast<uint32_t>((voice.sound->volume * volume_ * state.volume) >> 14);
    if (library_form_ == AlPlayerForm::kPlayerScale)
    {
        channel = static_cast<uint32_t>((player_scale_ * volume_ * state.volume) >> 14);
        if (state.scale >= 0)
        {
            channel = (static_cast<uint32_t>(state.scale) * channel) >> 7;
        }
    }

    note *= channel;
    note >>= 15;

    return static_cast<int16_t>(note);
}

uint8_t AlPlayer::FxMix(const ChannelState& channel) const
{
    return OffsetFxMix(channel.fx_mix, fx_offset_);
}

void AlPlayer::ChangeVoiceLevel(AlSynth& synth, int voice)
{
    if (library_form_ == AlPlayerForm::kPlayerScale && RestartDecay(synth, voice))
    {
        return;
    }

    const VoiceState& state = voices_[static_cast<std::size_t>(voice)];
    synth.SetVolume(state.synth_voice, VoiceVolume(state), VoiceDelta(state));
}

bool AlPlayer::RestartDecay(AlSynth& synth, int voice)
{
    VoiceState& state = voices_[static_cast<std::size_t>(voice)];
    const int32_t left = state.envelope_end - time_;
    if (state.envelope_phase != EnvelopePhase::kDecay || left <= kGainChangeTime)
    {
        return false;
    }

    const BankEnvelope& envelope = file_->envelopes[static_cast<std::size_t>(state.sound->envelope)];
    const auto attack = static_cast<float>(envelope.attack_volume);
    const float rate = DecayRate(attack, static_cast<float>(state.envelope_gain), envelope.decay_time);
    const float gain = DecayGain(attack, envelope.decay_time - left + kGainChangeTime, rate);
    state.envelope_gain = static_cast<uint8_t>(static_cast<int32_t>(gain));
    synth.SetVolume(state.synth_voice, VoiceVolume(state), kGainChangeTime);
    state.envelope_phase = EnvelopePhase::kAttack;

    Event decay;
    decay.type = EventType::kEnvelope;
    decay.voice = voice;
    decay.volume = envelope.decay_volume;
    decay.delta = left - kGainChangeTime;
    Post(decay, kGainChangeTime);

    return true;
}

int32_t AlPlayer::VoiceDelta(const VoiceState& voice) const
{
    const int32_t delta = voice.envelope_end - time_;

    return delta >= 0 ? delta : kGainChangeTime;
}

uint8_t AlPlayer::VoicePan(const VoiceState& voice) const
{
    const int pan = channels_state_[voice.channel].pan - kPanCenter + voice.sound->pan;

    return static_cast<uint8_t>(std::clamp(pan, 0, 127));
}

AlWave AlPlayer::WaveOf(const BankSound& sound) const
{
    return WaveOfSound({file_, bank_, file_address_, sample_address_}, sound);
}

void AlPlayer::InitFromBank()
{
    const Bank& bank = file_->banks[bank_];
    const BankInstrument* first = nullptr;
    for (const int32_t index : bank.instruments)
    {
        if (index != kNoObject)
        {
            first = &file_->instruments[static_cast<std::size_t>(index)];
            break;
        }
    }

    for (int i = 0; i < channels_; i++)
    {
        ResetChannel(i);
        if (first != nullptr)
        {
            SetInstrument(*first, i);
        }
    }

    if (bank.percussion != kNoObject && channels_ > 9)
    {
        SetInstrument(file_->instruments[static_cast<std::size_t>(bank.percussion)], 9);
    }
}

void AlPlayer::SetInstrument(const BankInstrument& instrument, int channel)
{
    ChannelState& state = channels_state_[static_cast<std::size_t>(channel)];
    state.instrument = &instrument;
    state.pan = instrument.pan;
    state.volume = instrument.volume;
    state.priority = instrument.priority;
    state.bend_range = instrument.bend_range;
}

void AlPlayer::ResetChannel(int channel)
{
    ChannelState& state = channels_state_[static_cast<std::size_t>(channel)];
    state.fx_mix = 0;
    state.pan = kPanCenter;
    state.volume = library_form_ == AlPlayerForm::kPlayerScale ? kScaledFormVolume : kVolumeFull;
    state.priority = kDefaultPriority;
    state.sustain = 0;
    state.bend_range = 200;
    state.pitch_bend = 1.0F;
}

void AlPlayer::SetTempo(float tempo)
{
    microseconds_per_tick_ =
        has_sequence_ ? static_cast<int32_t>(tempo * ticks_per_quarter_inverse_) : kInitialTickTime;
}

} // namespace ultralib::n64
