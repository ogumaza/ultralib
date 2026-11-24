// SPDX-License-Identifier: MIT

// Small helpers that the models of the EAD engines and their conversion share: the reading of a script's bytes, the
// flow commands of the scripts and the player commands that the engines have in common, the lookups in a bank, the
// table of the voices' settings for each update of a frame, the test for players or voices that still run, the headset
// delay and the length of an update.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "ultralib/n64/bank_file.h"
#include "ultralib/n64/ead_bank.h"

namespace ultralib::n64::ead_common
{

// The byte at `place` in a sequence's bytes. A place past their end reads as 0.
inline uint8_t PeekByte(const std::vector<uint8_t>& bytes, uint32_t place)
{
    return place < bytes.size() ? bytes[place] : uint8_t{0};
}

// The big-endian halfword at `place`.
inline uint16_t Peek16(const std::vector<uint8_t>& bytes, uint32_t place)
{
    return static_cast<uint16_t>((uint32_t{PeekByte(bytes, place)} << 8) | PeekByte(bytes, place + 1));
}

// Reads a variable length at `pc` and moves `pc` past it: one byte, or two when the top bit of the first is set. The
// second form contains 15 bits, the high bits first.
inline uint16_t ReadVariable(const std::vector<uint8_t>& bytes, uint32_t& pc)
{
    const uint8_t first = PeekByte(bytes, pc++);
    if ((first & 0x80) == 0)
    {
        return first;
    }

    return static_cast<uint16_t>(((first & 0x7F) << 8) | PeekByte(bytes, pc++));
}

// The flow commands of a script in every engine. A script has a stack of four return places and four loop counts. The
// low two bits of its depth index them.

// Command 0xFF: returns to the place on top of the stack. Returns false at depth 0. The script ends there.
template <typename Script>
bool ReturnFromCall(Script& script)
{
    if (script.depth == 0)
    {
        return false;
    }

    script.depth--;
    script.pc = script.stack[script.depth & 3];

    return true;
}

// Command 0xFC, and the calls through a table: pushes the place after the command and goes to `target`.
template <typename Script>
void Call(Script& script, uint32_t target)
{
    script.stack[script.depth & 3] = script.pc;
    script.depth++;
    script.pc = target;
}

// Command 0xF8: starts a loop of `count` passes over the commands that follow.
template <typename Script>
void StartLoop(Script& script, uint8_t count)
{
    script.loops[script.depth & 3] = count;
    script.stack[script.depth & 3] = script.pc;
    script.depth++;
}

// Command 0xF7: goes back to the start of the loop on top of the stack, or leaves the loop after its last pass.
template <typename Script>
void EndLoop(Script& script)
{
    uint8_t& loops = script.loops[(script.depth - 1) & 3];
    loops--;
    if (loops != 0)
    {
        script.pc = script.stack[(script.depth - 1) & 3];
    }
    else
    {
        script.depth--;
    }
}

// Whether an absolute jump is taken for the script's `value`: 0xFB always, 0xFA when the value is 0, 0xF9 when it's
// negative and 0xF5 when it's at least 0.
inline bool JumpTaken(uint8_t command, int value)
{
    return !((command == 0xFA && value != 0) || (command == 0xF9 && value >= 0) || (command == 0xF5 && value < 0));
}

// Whether a relative jump of the middle and later engines is taken: 0xF4 always, 0xF3 when the value is 0 and 0xF2
// when it's negative.
inline bool RelativeJumpTaken(uint8_t command, int value)
{
    return !((command == 0xF3 && value != 0) || (command == 0xF2 && value >= 0));
}

// The place that a relative jump with the signed byte `offset` moves `pc` to.
inline uint32_t RelativeTarget(uint32_t pc, uint8_t offset)
{
    return static_cast<uint32_t>(static_cast<int32_t>(pc) + static_cast<int8_t>(offset));
}

// Runs a call (0xFC) or a loop (0xF8 and 0xF7) of a script in the early and middle engines. Returns false and reads
// nothing for any other command.
template <typename Script>
bool RunCallOrLoop(const std::vector<uint8_t>& bytes, Script& script, uint8_t command)
{
    switch (command)
    {
    case 0xFC:
        {
            const uint16_t target = Peek16(bytes, script.pc);
            script.pc += 2;
            Call(script, target);
            return true;
        }

    case 0xF8:
        StartLoop(script, PeekByte(bytes, script.pc++));
        return true;

    case 0xF7:
        EndLoop(script);
        return true;

    default:
        return false;
    }
}

// Runs a call, a loop or an absolute jump (0xF5 and 0xF9 to 0xFB) of a player's or channel's script in the early and
// middle engines, with `value` for the conditions. `relative_jumps` adds the relative jumps 0xF2 to 0xF4 of the middle
// engine. Returns false and reads nothing for any other command.
template <typename Script>
bool RunFlowCommand(const std::vector<uint8_t>& bytes, Script& script, uint8_t command, int value, bool relative_jumps)
{
    if (RunCallOrLoop(bytes, script, command))
    {
        return true;
    }

    switch (command)
    {
    case 0xFB:
    case 0xFA:
    case 0xF9:
    case 0xF5:
        {
            const uint16_t target = Peek16(bytes, script.pc);
            script.pc += 2;
            if (JumpTaken(command, value))
            {
                script.pc = target;
            }

            return true;
        }

    case 0xF4:
    case 0xF3:
    case 0xF2:
        {
            if (!relative_jumps)
            {
                return false;
            }

            const uint8_t offset = PeekByte(bytes, script.pc++);
            if (RelativeJumpTaken(command, value))
            {
                script.pc = RelativeTarget(script.pc, offset);
            }

            return true;
        }

    default:
        return false;
    }
}

// Runs a call, a loop or the jump 0xFB of a layer's script in the early and middle engines. `relative_jumps` adds the
// relative jump 0xF4 of the middle engine. A layer's jumps don't test a value. Returns false and reads nothing for any
// other command.
template <typename Script>
bool RunLayerFlowCommand(const std::vector<uint8_t>& bytes, Script& script, uint8_t command, bool relative_jumps)
{
    if (RunCallOrLoop(bytes, script, command))
    {
        return true;
    }

    switch (command)
    {
    case 0xFB:
        script.pc = Peek16(bytes, script.pc);
        return true;

    case 0xF4:
        {
            if (!relative_jumps)
            {
                return false;
            }

            const uint8_t offset = PeekByte(bytes, script.pc++);
            script.pc = RelativeTarget(script.pc, offset);
            return true;
        }

    default:
        return false;
    }
}

// Commands 0xD0 to 0xD5 of a player in every engine. They set the voice policy, the places of the tables of short gates
// and velocities, the behavior and level of a mute, and the mute itself. Does nothing for any other command.
template <typename Player, typename Script>
void SetPlayerField(Player& player, Script& script, uint8_t command)
{
    switch (command)
    {
    case 0xD5:
        player.mute_scale = static_cast<float>(static_cast<int8_t>(PeekByte(player.bytes, script.pc++))) / 127.0F;
        break;

    case 0xD4:
        player.muted = true;
        break;

    case 0xD3:
        player.mute_behavior = PeekByte(player.bytes, script.pc++);
        break;

    case 0xD2:
        player.short_velocities = Peek16(player.bytes, script.pc);
        script.pc += 2;
        break;

    case 0xD1:
        player.short_gates = Peek16(player.bytes, script.pc);
        script.pc += 2;
        break;

    case 0xD0:
        player.policy = PeekByte(player.bytes, script.pc++);
        break;

    default:
        break;
    }
}

// Command 0xDA of a player in the middle and later engines. Modes 0 and 1 set the length in updates of the next fade
// unless the player fades out (state 2). Mode 2 starts a fade out over `updates`.
template <typename Player>
void SetFade(Player& player, uint8_t mode, uint16_t updates)
{
    if (mode == 0 || mode == 1)
    {
        if (player.state != 2)
        {
            player.fade_updates_next = updates;
            player.state = mode;
        }
    }
    else if (mode == 2)
    {
        player.fade_updates = updates;
        player.state = mode;
        player.fade_step = (0.0F - player.fade_volume) / static_cast<float>(int32_t{updates});
    }
}

// Command 0xDB of a player in the middle and later engines: fades to `volume` (0 to 127) over the length that 0xDA
// set, or sets it at once for a length of 0. A player in state 1 starts its fade from silence.
template <typename Player>
void FadeTo(Player& player, int volume)
{
    if (player.state == 1)
    {
        player.state = 0;
        player.fade_volume = 0.0F;
    }

    if (player.state == 0)
    {
        player.fade_updates = player.fade_updates_next;
        if (player.fade_updates_next != 0)
        {
            player.fade_step = (static_cast<float>(volume) / 127.0F - player.fade_volume) /
                               static_cast<float>(int32_t{player.fade_updates});
        }
        else
        {
            player.fade_volume = static_cast<float>(volume) / 127.0F;
        }
    }
}

// The index in `bank.instruments` of a program's instrument, or kNoObject for a program without one.
inline int32_t ProgramObject(const EadBank& bank, int program)
{
    if (static_cast<std::size_t>(program) >= bank.program_instruments.size())
    {
        return kNoObject;
    }

    return bank.program_instruments[static_cast<std::size_t>(program)];
}

// The instrument of a program in a bank, or null for a program without one.
inline const EadInstrument* ProgramInstrument(const EadBank& bank, int program)
{
    const int32_t object = ProgramObject(bank, program);

    return object == kNoObject ? nullptr : &bank.instruments[static_cast<std::size_t>(object)];
}

// The index in `bank.drums` of a note's drum, or kNoObject for a note without one.
inline int32_t DrumObject(const EadBank& bank, int note)
{
    if (static_cast<std::size_t>(note) >= bank.drum_notes.size())
    {
        return kNoObject;
    }

    return bank.drum_notes[static_cast<std::size_t>(note)];
}

// The drum of a note in a bank, or null for a note without one.
inline const EadDrum* NoteDrum(const EadBank& bank, int note)
{
    const int32_t object = DrumObject(bank, note);

    return object == kNoObject ? nullptr : &bank.drums[static_cast<std::size_t>(object)];
}

// The program that the early engine's lookup finds: past the end of the bank's list, the last one; without an
// instrument, the nearest one below it that has one. Returns a negative number when there's none.
inline int NearestProgram(const EadBank& bank, int program)
{
    int chosen = std::min(program, static_cast<int>(bank.program_instruments.size()) - 1);
    while (chosen >= 0 && bank.program_instruments[static_cast<std::size_t>(chosen)] == kNoObject)
    {
        chosen--;
    }

    return chosen;
}

// The sample of a sound in bank `bank`, or null for no sound or a sound without a sample.
inline const EadSample* SoundSample(const std::vector<EadBank>& banks, int bank, const EadTunedSample* sound)
{
    if (sound == nullptr || sound->sample == kNoObject)
    {
        return nullptr;
    }

    return &banks[static_cast<std::size_t>(bank)].samples[static_cast<std::size_t>(sound->sample)];
}

// Turns off a voice's settings in the updates of the frame after `update`, up to the next update that starts a note on
// the voice. The middle and later engines keep a frame's settings in one table, the voices of each update in a row.
template <typename UpdateState>
void DisableLaterUpdates(std::vector<UpdateState>& states, std::size_t voices, int updates_per_frame, int voice,
                         int update)
{
    for (int later = update + 1; later < updates_per_frame; later++)
    {
        UpdateState& next = states[voices * static_cast<std::size_t>(later) + static_cast<std::size_t>(voice)];
        if (next.needs_init)
        {
            break;
        }

        next.enabled = false;
    }
}

// Whether any of `players` or `voices` is still enabled, in the middle and later engines. Their voices keep the flag in
// their state for an update.
template <typename Players, typename Voices>
bool AnyEnabled(const Players& players, const Voices& voices)
{
    return std::ranges::any_of(players, [](const auto& player) { return player.enabled; }) ||
           std::ranges::any_of(voices, [](const auto& voice) { return voice.state.enabled; });
}

// A headset delay of an update in the early and middle engines: the dry output that the delayed side mixes into, and
// the delays in bytes of this update and the one before.
struct HeadsetDelay
{
    uint32_t target = 0;
    uint32_t shift = 0;
    uint32_t previous_shift = 0;
};

// Chooses the delay of a voice for `side`, and moves the voice's record of its previous delays on. Side 1 delays the
// output at `left` by `delay_right`. Side 2 delays the output at `right` by `delay_left`. Returns nothing for side 0.
template <typename Previous>
std::optional<HeadsetDelay> ChooseHeadsetDelay(int side, uint32_t left, uint32_t right, uint32_t delay_left,
                                               uint32_t delay_right, Previous& previous_left, Previous& previous_right)
{
    HeadsetDelay delay;
    if (side == 1)
    {
        delay.target = left;
        delay.shift = delay_right;
        previous_left = 0;
        delay.previous_shift = previous_right;
        previous_right = static_cast<Previous>(delay.shift);
    }
    else if (side == 2)
    {
        delay.target = right;
        delay.shift = delay_left;
        previous_right = 0;
        delay.previous_shift = previous_left;
        previous_left = static_cast<Previous>(delay.shift);
    }
    else
    {
        return std::nullopt;
    }

    return delay;
}

// The pitch of the resampling that stretches an update of `bytes` from the previous headset delay to the new one. A
// small change of rate fits a change of the delay.
inline uint32_t HeadsetResamplePitch(uint32_t bytes, const HeadsetDelay& delay)
{
    uint32_t divisor = bytes + delay.shift - delay.previous_shift;
    if (delay.previous_shift == 0)
    {
        divisor += 8;
    }
    else if (delay.shift == 0)
    {
        divisor = bytes - delay.previous_shift - 4;
    }

    return ((bytes << 15) / divisor) & 0xFFFF;
}

// The samples of the next update of a frame in the middle and later engines. The last update takes all `remaining`
// samples. Any other takes the longest size when an even share of them for the `left` updates reaches it, the shortest
// size when the share is at most the shortest, and the usual size otherwise. Sets `size` to 0, 1 or 2 for the shortest,
// usual or longest size, and leaves it for the last update.
inline int UpdateLength(int remaining, int left, int shortest, int usual, int longest, int* size = nullptr)
{
    if (left == 1)
    {
        return remaining;
    }

    int chosen = 1;
    int length = usual;
    if (remaining / left >= longest)
    {
        chosen = 2;
        length = longest;
    }
    else if (remaining / left <= shortest)
    {
        chosen = 0;
        length = shortest;
    }

    if (size != nullptr)
    {
        *size = chosen;
    }

    return length;
}

} // namespace ultralib::n64::ead_common
