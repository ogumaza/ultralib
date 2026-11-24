// SPDX-License-Identifier: MIT

// The voices of the SBN engine on the engine side: their envelopes, their volumes and what they send to the synthesis
// driver.

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "sbn_player_constants.h"
#include "ultralib/n64/sbn.h"
#include "ultralib/n64/sbn_player.h"

namespace ultralib::n64
{

namespace
{

using sbn_player_constants::kChunk;
using sbn_player_constants::kEndCommand;
using sbn_player_constants::kEnvHandledChange;
using sbn_player_constants::kEnvKeyReleased;
using sbn_player_constants::kEnvReleasing;
using sbn_player_constants::kEnvVolumeChanged;
using sbn_player_constants::kFrameMicroseconds;
using sbn_player_constants::kSyncParams;

constexpr uint8_t kEnvMax = 0x80;

int32_t Multiply4(int32_t a, int32_t b, int32_t c, int32_t d)
{
    return (((a * b * c) >> 7 >> 7) * d) >> 7;
}

int32_t GetDelta(int32_t microseconds)
{
    return (microseconds / kFrameMicroseconds) * kChunk;
}

} // namespace

void SbnPlayer::ResetVoice(int voice)
{
    Voice& held = voices_[static_cast<std::size_t>(voice)];
    held.has_commands = false;
    held.done_pending = true;
    held.sync_flags = 0;
    SynthSetVolumeDelta(voice, 0, kChunk);
}

void SbnPlayer::ResetAllVoices(const Player& player)
{
    for (int i = 0; i < kVoices; i++)
    {
        if (voices_[static_cast<std::size_t>(i)].priority == player.priority)
        {
            ResetVoice(i);
        }
    }
}

int32_t SbnPlayer::IntervalTime(uint8_t interval) const
{
    const std::vector<int32_t>& times = config_.tables.envelope_times;

    return interval < times.size() ? times[interval] : 0;
}

uint8_t SbnPlayer::StepEnvelope(Voice& voice)
{
    while (true)
    {
        const uint8_t op = ReadEnvelope(voice.commands);
        if (op < 0x80)
        {
            return op;
        }

        const uint8_t argument = ReadEnvelope(voice.commands);
        switch (op)
        {
        case 0xFE:
            voice.scale = std::min<int32_t>(argument, kEnvMax);
            break;

        case 0xFD:
            voice.scale = std::clamp<int32_t>(voice.scale + static_cast<int8_t>(argument), 0, kEnvMax);
            break;

        case 0xFC:
            voice.loop_counter = argument;
            voice.loop_start = voice.commands;
            break;

        case 0xFB:
            if (voice.loop_counter == 0 || --voice.loop_counter != 0)
            {
                voice.commands = voice.loop_start;
            }
            break;

        default:
            break;
        }
    }
}

void SbnPlayer::StartEnvelope(Voice& voice)
{
    voice.commands = voice.envelope.press;
    voice.has_commands = true;
    voice.scale = kEnvMax;
    const uint8_t interval = StepEnvelope(voice);
    voice.envelope_flags = 0;
    voice.initial = 0;
    voice.target = ReadEnvelope(voice.commands);
    voice.interval = interval;
    voice.duration = IntervalTime(interval);
    voice.time_left = voice.duration;
    voice.volume = static_cast<int16_t>((voice.target * voice.client_volume * voice.scale) >> 7 >> 7);
    voice.delta = GetDelta(voice.duration);
    voice.envelope_delta = voice.duration != 0
                               ? (static_cast<float>(voice.target) - static_cast<float>(voice.initial)) /
                                     static_cast<float>(voice.duration)
                               : 0.0F;
    voice.relative_release = false;
    voice.relative_start = kEnvMax;
}

void SbnPlayer::AfterVolumeChange(Voice& voice)
{
    voice.volume =
        static_cast<int16_t>(Multiply4(voice.target, voice.client_volume, voice.relative_start, voice.scale));
    voice.delta = GetDelta(voice.time_left);
    voice.envelope_flags = static_cast<uint8_t>(voice.envelope_flags & ~kEnvHandledChange);
    voice.sync_flags |= kSyncParams;
}

void SbnPlayer::UpdateVoices()
{
    for (Voice& voice : voices_)
    {
        if (!voice.has_commands)
        {
            continue;
        }

        if ((voice.envelope_flags & kEnvHandledChange) != 0)
        {
            AfterVolumeChange(voice);
        }
        else if ((voice.envelope_flags & kEnvKeyReleased) != 0)
        {
            ReleaseEnvelope(voice);
        }
        else if (voice.time_left == -1)
        {
            if ((voice.envelope_flags & kEnvVolumeChanged) != 0)
            {
                voice.envelope_flags = static_cast<uint8_t>(voice.envelope_flags & ~kEnvVolumeChanged);
                SetEnvelopeVolume(voice, voice.initial);
            }
        }
        else
        {
            StepVoiceEnvelope(voice);
        }
    }
}

// Returns the envelope level of a voice at the current time of the segment.
int32_t SbnPlayer::EnvelopeLevel(const Voice& voice)
{
    return voice.initial +
           static_cast<int32_t>(voice.envelope_delta * static_cast<float>(voice.duration - voice.time_left));
}

// Applies a change of the client volume of a voice during a segment. Returns the envelope level to use.
int32_t SbnPlayer::ApplyVolumeChange(Voice& voice)
{
    int32_t current = 0;
    voice.envelope_flags = static_cast<uint8_t>(voice.envelope_flags & ~kEnvVolumeChanged);
    if (voice.time_left > kFrameMicroseconds)
    {
        voice.time_left -= kFrameMicroseconds;
        voice.envelope_flags |= kEnvHandledChange;
        current = EnvelopeLevel(voice);
    }
    else
    {
        current = voice.target;
    }

    voice.delta = kChunk;

    return current;
}

// Sets the volume of a voice from an envelope level and marks the voice for a mixer sync.
void SbnPlayer::SetEnvelopeVolume(Voice& voice, int32_t current)
{
    voice.volume = static_cast<int16_t>(Multiply4(current, voice.client_volume, voice.relative_start, voice.scale));
    voice.sync_flags |= kSyncParams;
}

// Sets the volume and the ramp length of a voice at the start of an envelope segment.
void SbnPlayer::SetSegmentVolume(Voice& voice)
{
    int32_t current = 0;
    if ((voice.envelope_flags & kEnvVolumeChanged) != 0)
    {
        current = ApplyVolumeChange(voice);
    }
    else
    {
        voice.delta = GetDelta(voice.duration);
        current = voice.target;
    }

    SetEnvelopeVolume(voice, current);
}

// Starts the release envelope of a voice whose key has been released.
void SbnPlayer::ReleaseEnvelope(Voice& voice)
{
    voice.envelope_flags = static_cast<uint8_t>((voice.envelope_flags & ~kEnvKeyReleased) | kEnvReleasing);
    voice.commands = voice.envelope.release;
    voice.initial = voice.time_left > kFrameMicroseconds ? EnvelopeLevel(voice) : voice.target;
    voice.interval = ReadEnvelope(voice.commands);
    uint8_t value = ReadEnvelope(voice.commands);
    if ((value & 0x80) != 0)
    {
        value &= 0x7F;
        voice.relative_release = true;
        voice.relative_start = voice.initial;
    }

    voice.target = value;
    voice.time_left = IntervalTime(voice.interval);
    voice.duration = voice.time_left;
    SetSegmentVolume(voice);
}

// Steps the envelope of a voice by a chunk. Starts the next segment when the current one ends. Holds or ends the
// envelope at the end of the commands.
void SbnPlayer::StepVoiceEnvelope(Voice& voice)
{
    voice.time_left -= kFrameMicroseconds;
    if (voice.time_left > 0)
    {
        if ((voice.envelope_flags & kEnvVolumeChanged) != 0)
        {
            const int32_t current = ApplyVolumeChange(voice);
            SetEnvelopeVolume(voice, current);
        }

        return;
    }

    if (EnvelopeByte(voice.commands) != kEndCommand)
    {
        voice.interval = StepEnvelope(voice);
        voice.initial = voice.target;
        voice.target = ReadEnvelope(voice.commands) & 0x7F;
        voice.time_left = IntervalTime(voice.interval);
        voice.duration = voice.time_left;
        voice.envelope_delta = voice.duration != 0
                                   ? (static_cast<float>(voice.target) - static_cast<float>(voice.initial)) /
                                         static_cast<float>(voice.duration)
                                   : 0.0F;
        SetSegmentVolume(voice);

        return;
    }

    if ((voice.envelope_flags & kEnvReleasing) != 0)
    {
        voice.envelope_flags = 0;
        voice.has_commands = false;
        voice.done_pending = true;
    }
    else
    {
        voice.time_left = -1;
        voice.duration = -1;
        voice.delta = kChunk;
        voice.envelope_delta = 0.0F;
        voice.initial = voice.target;
    }
}

} // namespace ultralib::n64
