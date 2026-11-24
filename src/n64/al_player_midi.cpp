// SPDX-License-Identifier: MIT

// The channel messages of libultra's sequence players: notes, pitch bends, controllers, programs and tempos.

#include <cstddef>
#include <cstdint>

#include "al_player_constants.h"
#include "ultralib/n64/al_player.h"
#include "ultralib/n64/al_synth.h"

namespace ultralib::n64
{

namespace
{

using al_player_constants::kStatusChannelPressure;
using al_player_constants::kStatusController;
using al_player_constants::kStatusNoteOff;
using al_player_constants::kStatusNoteOn;
using al_player_constants::kStatusPitchBend;
using al_player_constants::kStatusPolyPressure;
using al_player_constants::kStatusProgram;
using al_player_constants::kVolumeFull;
using al_player_constants::OffsetFxMix;

constexpr uint8_t kSustainThreshold = 63;

constexpr uint8_t kControllerVolume = 0x07;
constexpr uint8_t kControllerChannelScale = 0x08;
constexpr uint8_t kControllerPlayerScale = 0x15;
constexpr uint8_t kControllerPan = 0x0A;
constexpr uint8_t kControllerPriority = 0x10;
constexpr uint8_t kControllerSustain = 0x40;
constexpr uint8_t kControllerFxMix = 0x5B;

} // namespace

void AlPlayer::HandleMidi(AlSynth& synth, const Event& event)
{
    const auto status = static_cast<uint8_t>(event.status & 0xF0);
    const auto channel_number = static_cast<uint8_t>(event.status & 0x0F);
    const uint8_t byte1 = event.byte1;
    const uint8_t byte2 = event.byte2;
    if (channel_number >= channels_state_.size())
    {
        return;
    }

    const bool note_on = status == kStatusNoteOn && byte2 != 0;
    if (note_on)
    {
        StartNote(synth, event, channel_number);

        return;
    }

    switch (status)
    {
    case kStatusNoteOn:
    case kStatusNoteOff:
        ReleaseNote(synth, channel_number, byte1);
        break;

    case kStatusPolyPressure:
        {
            const int index = LookupVoice(byte1, channel_number);
            if (index >= 0)
            {
                VoiceState& voice = voices_[static_cast<std::size_t>(index)];
                voice.velocity = byte2;
                synth.SetVolume(voice.synth_voice, VoiceVolume(voice), VoiceDelta(voice));
            }
            break;
        }

    case kStatusChannelPressure:
        for (const int index : active_voices_)
        {
            VoiceState& voice = voices_[static_cast<std::size_t>(index)];
            if (voice.channel == channel_number)
            {
                voice.velocity = byte1;
                synth.SetVolume(voice.synth_voice, VoiceVolume(voice), VoiceDelta(voice));
            }
        }
        break;

    case kStatusController:
        HandleController(synth, channel_number, byte1, byte2);
        break;

    case kStatusProgram:
        {
            const std::vector<int32_t>& instruments = file_->banks[bank_].instruments;
            if (byte1 < instruments.size() && instruments[byte1] != kNoObject)
            {
                SetInstrument(file_->instruments[static_cast<std::size_t>(instruments[byte1])], channel_number);
            }
            break;
        }

    case kStatusPitchBend:
        SetPitchBend(synth, channel_number, byte1, byte2);
        break;

    default:
        break;
    }
}

// A note off: the release of the voice that plays the key, or its wait for the sustain pedal.
void AlPlayer::ReleaseNote(AlSynth& synth, uint8_t channel_number, uint8_t byte1)
{
    const int index = LookupVoice(byte1, channel_number);
    if (index < 0)
    {
        return;
    }

    VoiceState& voice = voices_[static_cast<std::size_t>(index)];
    if (voice.phase == NotePhase::kSustain)
    {
        voice.phase = NotePhase::kSustainRelease;
    }
    else
    {
        voice.phase = NotePhase::kRelease;
        const BankEnvelope& envelope = file_->envelopes[static_cast<std::size_t>(voice.sound->envelope)];
        ReleaseVoice(synth, index, envelope.release_time);
    }
}

// A pitch bend of a channel, on each of its voices.
void AlPlayer::SetPitchBend(AlSynth& synth, uint8_t channel_number, uint8_t byte1, uint8_t byte2)
{
    ChannelState& channel = channels_state_[channel_number];
    const int32_t bend = (byte2 << 7) + byte1 - 8192;
    const int32_t cents = (channel.bend_range * bend) / 8192;
    const float ratio = CentsToRatio(cents);
    channel.pitch_bend = ratio;
    for (const int index : active_voices_)
    {
        const VoiceState& voice = voices_[static_cast<std::size_t>(index)];
        if (voice.channel == channel_number)
        {
            synth.SetPitch(voice.synth_voice, voice.pitch * ratio * voice.vibrato);
        }
    }
}

// Starts a note: a voice with the sound that the key and velocity pick, its envelope and its oscillators.
void AlPlayer::StartNote(AlSynth& synth, const Event& event, uint8_t channel_number)
{
    const uint8_t byte1 = event.byte1;
    const uint8_t byte2 = event.byte2;
    const ChannelState& channel = channels_state_[channel_number];
    if (state_ != AlPlayerState::kPlaying)
    {
        return;
    }

    const BankSound* sound = LookupSound(byte1, byte2, channel_number);
    if (sound == nullptr)
    {
        return;
    }

    const int index = MapVoice(byte1, byte2, channel_number);
    if (index < 0)
    {
        return;
    }

    VoiceState& voice = voices_[static_cast<std::size_t>(index)];
    synth.AllocVoice(voice.synth_voice, channel.priority, false);
    synth.SetVoiceTag(voice.synth_voice, channel_number);
    const BankEnvelope& envelope = file_->envelopes[static_cast<std::size_t>(sound->envelope)];
    const BankKeyMap& key_map = file_->key_maps[static_cast<std::size_t>(sound->key_map)];
    voice.sound = sound;
    voice.envelope_phase = EnvelopePhase::kAttack;
    voice.phase = channel.sustain > kSustainThreshold ? NotePhase::kSustain : NotePhase::kNoteOn;
    const auto cents = static_cast<int16_t>((byte1 - key_map.key_base) * 100 + key_map.detune);
    voice.pitch = CentsToRatio(cents);
    voice.envelope_gain = envelope.attack_volume;
    voice.envelope_end = time_ + envelope.attack_time;
    StartVoiceOscillators(index, channel);
    const float pitch = voice.pitch * channel.pitch_bend * voice.vibrato;
    synth.StartVoiceParams(voice.synth_voice, WaveOf(*sound), pitch, VoiceVolume(voice), VoicePan(voice),
                           FxMix(channel), envelope.attack_time);

    Event decay;
    decay.type = EventType::kEnvelope;
    decay.voice = index;
    decay.volume = envelope.decay_volume;
    decay.delta = envelope.decay_time;
    Post(decay, envelope.attack_time);

    if (form_ == AlSequenceForm::kCompressed && event.duration != 0)
    {
        Event note_off;
        note_off.type = EventType::kNoteOff;
        note_off.status = static_cast<uint8_t>(channel_number | kStatusNoteOff);
        note_off.byte1 = byte1;
        Post(note_off, microseconds_per_tick_ * static_cast<int32_t>(event.duration));
    }

    if (on_note)
    {
        on_note({channel_number, byte1, byte2, event.duration, event.offset, time_});
    }
}

// The tremolo and the vibrato of a new voice, from the instrument of its channel.
void AlPlayer::StartVoiceOscillators(int index, const ChannelState& channel)
{
    VoiceState& voice = voices_[static_cast<std::size_t>(index)];
    voice.tremolo = kVolumeFull;
    voice.vibrato = 1.0F;
    voice.oscillators = 0;
    if (oscillators_ && channel.instrument != nullptr)
    {
        const BankInstrument& instrument = *channel.instrument;
        float value = static_cast<float>(kVolumeFull);
        if (instrument.tremolo_type != 0)
        {
            Event wave;
            wave.type = EventType::kTremolo;
            wave.voice = index;
            const int32_t delay =
                StartOscillator(wave.oscillator, value, instrument.tremolo_type, instrument.tremolo_rate,
                                instrument.tremolo_depth, instrument.tremolo_delay);
            if (delay != 0)
            {
                Post(wave, delay);
                voice.oscillators |= 1;
            }
        }

        voice.tremolo = static_cast<uint8_t>(static_cast<int32_t>(value));
        value = 1.0F;
        if (instrument.vibrato_type != 0)
        {
            Event wave;
            wave.type = EventType::kVibrato;
            wave.voice = index;
            const int32_t delay =
                StartOscillator(wave.oscillator, value, instrument.vibrato_type, instrument.vibrato_rate,
                                instrument.vibrato_depth, instrument.vibrato_delay);
            if (delay != 0)
            {
                Post(wave, delay);
                voice.oscillators |= 2;
            }
        }

        voice.vibrato = value;
    }
}

// A controller's change on a channel.
void AlPlayer::HandleController(AlSynth& synth, uint8_t channel_number, uint8_t byte1, uint8_t byte2)
{
    ChannelState& channel = channels_state_[channel_number];
    switch (byte1)
    {
    case kControllerPan:
        channel.pan = byte2;
        ApplyChannelPan(synth, channel_number);
        break;

    case kControllerVolume:
        channel.volume = byte2;
        ApplyChannelLevel(synth, channel_number);
        break;

    case kControllerChannelScale:
        if (library_form_ == AlPlayerForm::kPlayerScale)
        {
            channel.scale = static_cast<int8_t>(byte2);
            ApplyChannelLevel(synth, channel_number);
        }
        break;

    case kControllerPlayerScale:
        if (library_form_ == AlPlayerForm::kPlayerScale)
        {
            player_scale_ = byte2;
            for (const int index : active_voices_)
            {
                RestartDecay(synth, index);
            }
        }
        break;

    case kControllerPriority:
        channel.priority = byte2;
        break;

    case kControllerSustain:
        channel.sustain = byte2;
        ApplySustain(synth, channel_number, byte2);
        break;

    case kControllerFxMix:
        channel.fx_mix = byte2;
        ApplyFxMix(synth, channel_number, byte2);
        break;

    default:
        break;
    }
}

// The pan of a channel, on each of its voices.
void AlPlayer::ApplyChannelPan(AlSynth& synth, uint8_t channel_number)
{
    for (const int index : active_voices_)
    {
        const VoiceState& voice = voices_[static_cast<std::size_t>(index)];
        if (voice.channel == channel_number)
        {
            synth.SetPan(voice.synth_voice, VoicePan(voice));
            if (library_form_ == AlPlayerForm::kPlayerScale)
            {
                RestartDecay(synth, index);
            }
        }
    }
}

// The level of a channel, on each of its voices that isn't in its release.
void AlPlayer::ApplyChannelLevel(AlSynth& synth, uint8_t channel_number)
{
    for (const int index : active_voices_)
    {
        const VoiceState& voice = voices_[static_cast<std::size_t>(index)];
        if (voice.channel == channel_number && voice.envelope_phase != EnvelopePhase::kRelease)
        {
            ChangeVoiceLevel(synth, index);
        }
    }
}

// The sustain pedal of a channel, on each of its voices.
void AlPlayer::ApplySustain(AlSynth& synth, uint8_t channel_number, uint8_t byte2)
{
    for (const int index : std::vector<int>(active_voices_))
    {
        VoiceState& voice = voices_[static_cast<std::size_t>(index)];
        if (voice.channel != channel_number || voice.phase == NotePhase::kRelease)
        {
            continue;
        }

        if (byte2 > kSustainThreshold)
        {
            if (voice.phase == NotePhase::kNoteOn)
            {
                voice.phase = NotePhase::kSustain;
            }
        }
        else if (voice.phase == NotePhase::kSustain)
        {
            voice.phase = NotePhase::kNoteOn;
        }
        else if (voice.phase == NotePhase::kSustainRelease)
        {
            voice.phase = NotePhase::kRelease;
            const BankEnvelope& envelope = file_->envelopes[static_cast<std::size_t>(voice.sound->envelope)];
            ReleaseVoice(synth, index, envelope.release_time);
        }
    }
}

// The effect mix of a channel, on its voices.
void AlPlayer::ApplyFxMix(AlSynth& synth, uint8_t channel_number, uint8_t byte2)
{
    // The offset adds up over the channel's voices. Pokémon Snap's player sets the mix on every voice.
    uint8_t mix = byte2;
    for (const int index : active_voices_)
    {
        const VoiceState& voice = voices_[static_cast<std::size_t>(index)];
        if (voice.channel == channel_number)
        {
            mix = OffsetFxMix(mix, fx_offset_);
            synth.SetFxMix(voice.synth_voice, mix);
        }
        else if (library_form_ == AlPlayerForm::kPlayerScale)
        {
            synth.SetFxMix(voice.synth_voice, mix);
        }
    }
}

void AlPlayer::HandleTempo(const SequenceEvent& event)
{
    const int32_t old_tick_time = microseconds_per_tick_;
    const int32_t tempo = (event.byte1 << 16) | (event.byte2 << 8) | event.byte3;
    SetTempo(static_cast<float>(tempo));
    if (form_ != AlSequenceForm::kCompressed)
    {
        return;
    }

    // The queued note offs keep their ticks. Each goes back in the queue at the new tempo, with the last taken first.
    std::vector<QueueItem> note_offs;
    int32_t current = 0;
    for (std::size_t i = 0; i < queue_.size();)
    {
        current += queue_[i].delta;
        if (queue_[i].event.type == EventType::kNoteOff)
        {
            QueueItem item = queue_[i];
            const int32_t delta = item.delta;
            item.delta = current;
            if (i + 1 < queue_.size())
            {
                current -= delta;
                queue_[i + 1].delta += delta;
            }

            queue_.erase(queue_.begin() + static_cast<std::ptrdiff_t>(i));

            // The first stays first. Each later one goes just after it. The rest therefore run in reverse.
            if (note_offs.empty())
            {
                note_offs.push_back(item);
            }
            else
            {
                note_offs.insert(note_offs.begin() + 1, item);
            }
        }
        else
        {
            i++;
        }
    }

    for (QueueItem& item : note_offs)
    {
        const auto ticks = static_cast<uint32_t>(item.delta) / static_cast<uint32_t>(old_tick_time);
        int32_t delta = static_cast<int32_t>(ticks) * microseconds_per_tick_;
        bool placed = false;
        for (std::size_t i = 0; i < queue_.size(); i++)
        {
            if (delta < queue_[i].delta)
            {
                queue_[i].delta -= delta;
                queue_.insert(queue_.begin() + static_cast<std::ptrdiff_t>(i), QueueItem{item.event, delta});
                placed = true;
                break;
            }

            delta -= queue_[i].delta;
        }

        if (!placed)
        {
            queue_.push_back({item.event, delta});
        }
    }
}

} // namespace ultralib::n64
