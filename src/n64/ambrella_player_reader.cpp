// SPDX-License-Identifier: MIT

// The reader of the player's sequences: the bytes of the file, the running status, the held notes, the pedals and the
// loops.

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "ambrella_player_constants.h"
#include "ultralib/n64/ambrella_player.h"

namespace ultralib::n64
{

namespace
{

using ambrella_player_constants::kControllerLoopEnd;
using ambrella_player_constants::kControllerLoopStart;
using ambrella_player_constants::kControllerSustain;
using ambrella_player_constants::kDeviceControl;
using ambrella_player_constants::kFadeMessage;
using ambrella_player_constants::kFadeTimeUnits;
using ambrella_player_constants::kMaximum7Bit;
using ambrella_player_constants::kMetaEnd;
using ambrella_player_constants::kMetaTempo;
using ambrella_player_constants::kMicroseconds;
using ambrella_player_constants::kPedalDown;
using ambrella_player_constants::kStatusController;
using ambrella_player_constants::kStatusMeta;
using ambrella_player_constants::kStatusNoteOff;
using ambrella_player_constants::kStatusNoteOn;
using ambrella_player_constants::kStatusProgram;
using ambrella_player_constants::kStatusSystem;
using ambrella_player_constants::kStatusSystemEnd;
using ambrella_player_constants::kUniversalRealTime;

uint8_t To7Bit(double value)
{
    const auto whole = static_cast<int32_t>(value);

    return static_cast<uint8_t>(std::clamp(whole, 0, int32_t{kMaximum7Bit}));
}

} // namespace

uint8_t AmbrellaPlayer::NextByte(Player& player)
{
    return player.position < player.file.size() ? player.file[player.position++] : uint8_t{0};
}

uint32_t AmbrellaPlayer::NextLength(Player& player)
{
    uint32_t value = NextByte(player);
    if ((value & 0x80) != 0)
    {
        value &= 0x7F;
        uint8_t byte = 0;
        do
        {
            byte = NextByte(player);
            value = (value << 7) + (byte & 0x7F);
        } while ((byte & 0x80) != 0);
    }

    return value;
}

void AmbrellaPlayer::StopReader(Player& player)
{
    if (!player.stopped)
    {
        player.flush_notes = true;
        player.flush_pedals = true;
        player.stopped = true;
    }
}

// The code at 0x8000530C.
bool AmbrellaPlayer::ReadFromFile(Player& player, double& delta)
{
    delta = 0.0;
    for (;;)
    {
        if (FlushHeld(player))
        {
            return true;
        }

        if (player.stopped)
        {
            delta = -1.0;

            return false;
        }

        if (player.position >= player.end)
        {
            StopReader(player);
            continue;
        }

        const uint32_t ticks = ReadFileEvent(player);
        delta += static_cast<double>(ticks) * kMicroseconds * static_cast<double>(player.tempo) /
                 static_cast<double>(player.division) / player.speed;

        if (PassEvent(player))
        {
            return true;
        }
    }
}

// Makes the next event of a flush: a release of each pedal that is down, then a note on of velocity 0 for each held
// note. Returns false when the flush has no events left.
bool AmbrellaPlayer::FlushHeld(Player& player)
{
    auto& event = player.event;
    if (player.flush_pedals)
    {
        bool sent = false;
        for (int c = 0; c < kChannels && !sent; c++)
        {
            uint8_t& pedal = player.pedals[static_cast<std::size_t>(c)];
            if (pedal >= kPedalDown)
            {
                pedal = 0;
                event[0] = static_cast<uint8_t>(kStatusController | c);
                event[1] = kControllerSustain;
                event[2] = 0;
                sent = true;
            }
        }

        if (sent)
        {
            return true;
        }

        player.flush_pedals = false;
    }

    if (player.flush_notes)
    {
        if (player.held_notes != 0)
        {
            player.held_notes--;
            const auto held = static_cast<std::size_t>(player.held_notes);
            event[0] = static_cast<uint8_t>(kStatusNoteOn | player.held_channels[held]);
            event[1] = player.held_keys[held];
            event[2] = 0;

            return true;
        }

        player.flush_notes = false;
    }

    return false;
}

// Reads the next event of the file into `event`, with the running status where the file leaves out the status. Skips
// the bytes of a system exclusive message. Returns the ticks before the event.
uint32_t AmbrellaPlayer::ReadFileEvent(Player& player)
{
    auto& event = player.event;
    const uint32_t ticks = NextLength(player);
    player.event_offset = static_cast<uint32_t>(player.position);
    const uint8_t first = NextByte(player);
    event[0] = first;
    if (first == kStatusSystem || first == kStatusSystemEnd)
    {
        const uint32_t length = NextLength(player);
        player.position += length;
    }
    else if (first == kStatusMeta)
    {
        event[1] = NextByte(player);
        event[2] = NextByte(player);
        player.running_status = 0;
        const std::size_t length = event[2];
        for (std::size_t i = 0; i < length; i++)
        {
            const uint8_t byte = NextByte(player);
            if (i + 3 < event.size())
            {
                event[i + 3] = byte;
            }
        }
    }
    else
    {
        if ((first & 0x80) != 0)
        {
            event[1] = NextByte(player);
            player.running_status = first;
        }
        else
        {
            event[1] = first;
            event[0] = player.running_status;
        }

        const uint8_t kind = event[0] & 0xF0;
        if (kind != kStatusProgram && kind != 0xD0)
        {
            event[2] = NextByte(player);
        }
    }

    return ticks;
}

// Updates the held notes, pedals, loops and tempo for the event in `event`. Returns false for an event that the reader
// drops.
bool AmbrellaPlayer::PassEvent(Player& player)
{
    const auto& event = player.event;
    const uint8_t status = event[0];
    const uint8_t kind = status & 0xF0;
    const uint8_t channel = status & 0x0F;
    if (kind == kStatusNoteOn && event[2] != 0)
    {
        if (((player.channel_mask >> channel) & 1) == 0 || player.held_notes >= kHeldNotes)
        {
            return false;
        }

        const auto held = static_cast<std::size_t>(player.held_notes);
        player.held_channels[held] = channel;
        player.held_keys[held] = event[1];
        player.held_notes++;

        return true;
    }

    if (kind == kStatusNoteOff || kind == kStatusNoteOn)
    {
        for (int i = 0; i < player.held_notes; i++)
        {
            const auto held = static_cast<std::size_t>(i);
            if (player.held_channels[held] == channel && player.held_keys[held] == event[1])
            {
                player.held_notes--;
                const auto last = static_cast<std::size_t>(player.held_notes);
                player.held_channels[held] = player.held_channels[last];
                player.held_keys[held] = player.held_keys[last];
                break;
            }
        }

        return true;
    }

    if (kind == kStatusController)
    {
        return PassController(player, channel);
    }

    if (kind == kStatusSystem)
    {
        if (status == kStatusSystem || status == kStatusSystemEnd)
        {
            return false;
        }

        if (status == kStatusMeta)
        {
            if (event[1] == kMetaEnd)
            {
                StopReader(player);
            }
            else if (event[1] == kMetaTempo)
            {
                player.tempo = (event[3] << 16) | (event[4] << 8) | event[5];
            }

            return false;
        }
    }

    return true;
}

// Updates the pedals and the loop for a controller event. Returns false for a loop start or a loop end. The reader
// drops both.
bool AmbrellaPlayer::PassController(Player& player, uint8_t channel)
{
    const auto& event = player.event;
    if (event[1] == kControllerSustain)
    {
        player.pedals[channel] = event[2];
    }
    else if (event[1] == kControllerLoopStart)
    {
        player.loop_position = player.position;
        player.loop_status = player.running_status;
        player.has_loop = true;

        return false;
    }
    else if (event[1] == kControllerLoopEnd)
    {
        // From the sixth pass on, two passes of every three leave out a random set of the upper channels.
        player.flush_notes = true;
        if (player.passes % 3 > 0 && player.passes >= 4)
        {
            player.channel_mask = static_cast<uint16_t>((random ? random() : uint32_t{0xFFFF}) | 0xFF);
        }
        else
        {
            player.channel_mask = 0xFFFF;
        }

        player.passes++;
        if (player.has_loop)
        {
            player.position = player.loop_position;
            player.running_status = player.loop_status;
        }

        return false;
    }

    return true;
}

// The code at 0x800059E4 and 0x80006068.
bool AmbrellaPlayer::ReadEvent(Player& player)
{
    auto& event = player.event;
    if (player.stop_timer == 0.0)
    {
        StopReader(player);
        player.stop_timer = -1.0;
    }

    const auto fade_event = [&event](double seconds, double level)
    {
        event[0] = kStatusSystem;
        event[1] = kUniversalRealTime;
        event[2] = kUniversalRealTime;
        event[3] = kDeviceControl;
        event[4] = kFadeMessage;
        event[5] = To7Bit(seconds * kFadeTimeUnits);
        event[6] = To7Bit(level * kMaximum7Bit);
        event[7] = kStatusSystemEnd;
    };

    if (player.reset_pending)
    {
        player.delta = 0.0;
        event[0] = kStatusMeta;
        player.reset_pending = false;

        return true;
    }

    if (player.level_now >= 0.0)
    {
        fade_event(0.0, player.level_now);
        player.level_now = -1.0;
        player.delta = 0.0;

        return true;
    }

    if (player.fade_time >= 0.0)
    {
        fade_event(player.fade_time, player.fade_target);
        player.fade_time = -1.0;
        player.delta = 0.0;

        return true;
    }

    const bool read = ReadFromFile(player, player.delta);
    if (player.delta > 0.0 && player.stop_timer >= 0.0)
    {
        player.stop_timer = player.stop_timer < player.delta ? 0.0 : player.stop_timer - player.delta;
    }

    return read;
}

} // namespace ultralib::n64
