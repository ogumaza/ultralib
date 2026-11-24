// SPDX-License-Identifier: MIT

// The code that the two players of the N64 Music Tools share: the constants, the arithmetic, the readers of a
// channel's song and lists, and the commands that both forms run the same way. mus_files.cpp contains the readers of
// banks and songs, mus_player.cpp the early player and mus_late_player.cpp the late player. The conversion of the songs
// takes the commands' names and some of the constants from here.

// The addresses in the comments are places in the code of Pokémon Stadium. Where a comment gives a second address,
// that one is in the code of Pokémon Puzzle League, a game with the late player.

#pragma once

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "ultralib/n64/al_synth.h"
#include "ultralib/n64/mus_player.h"

namespace ultralib::n64::mus_player_common
{

// The commands of a song's lists. A byte below these is a note.
enum Command : uint8_t
{
    kStop = 0x80,
    kWave = 0x81,
    kPortamento = 0x82,
    kPortamentoOff = 0x83,
    kEnvelopeBytes = 0x84,
    kTempo = 0x85,
    kCutoff = 0x86,
    kEndIt = 0x87,
    kVibratoUp = 0x88,
    kVibratoDown = 0x89,
    kVibratoOff = 0x8A,
    kLength = 0x8B,
    kIgnoreLength = 0x8C,
    kTranspose = 0x8D,
    kIgnoreTranspose = 0x8E,
    kDistort = 0x8F,
    kEnvelope = 0x90,
    kEnvelopeOff = 0x91,
    kEnvelopeOn = 0x92,
    kTriggerOff = 0x93,
    kTriggerOn = 0x94,
    kFor = 0x95,
    kNext = 0x96,
    kWobble = 0x97,
    kWobbleOff = 0x98,
    kVelocityOn = 0x99,
    kVelocityOff = 0x9A,
    kDefaultVelocity = 0x9B,
    kPan = 0x9C,
    kStereo = 0x9D,
    kDrums = 0x9E,
    kDrumsOff = 0x9F,
    kPrint = 0xA0,
    kGoto = 0xA1,
    kReverb = 0xA2,
    kRandomNote = 0xA3,
    kRandomVolume = 0xA4,
    kRandomPan = 0xA5,
    kVolume = 0xA6,
    kStartEffect = 0xA7,
    kBendRange = 0xA8,
    kSweep = 0xA9,
    kChangeEffect = 0xAA,
    kMarker = 0xAB,
    kLengthOff = 0xAC,
};

inline constexpr int kRest = 96;
inline constexpr uint16_t kEndlessLength = 32767;
inline constexpr int kTicksShift = 8;
inline constexpr int kTick = 1 << kTicksShift;
inline constexpr int kMostCommandsInAStep = 100000;
inline constexpr int kReleasePhase = 4;
inline constexpr int kAttackPhase = 1;
inline constexpr int kDecayPhase = 2;
inline constexpr uint64_t kNeverOff = 0x7FFFFFFF;

// A loop that "for" opens with this count repeats for good.
inline constexpr uint8_t kForever = 0xFF;

// A channel's envelope bytes.
inline constexpr uint32_t kEnvelopeSize = 7;
inline constexpr uint32_t kEnvelopeAttack = 2;
inline constexpr uint32_t kEnvelopePeak = 3;
inline constexpr uint32_t kEnvelopeDecay = 4;
inline constexpr uint32_t kEnvelopeSustain = 5;
inline constexpr uint32_t kEnvelopeRelease = 6;

// The bytes of an entry in a song's table of drums.
inline constexpr uint32_t kDrumSize = 6;

inline constexpr double kVibratoUnitsPerSemitone = 50.0;
inline constexpr double kDistortUnitsPerSemitone = 100.0;
inline constexpr double kBendRangeUnit = 1.0 / 64.0;
inline constexpr float kDefaultBendRange = 1.0F / 32.0F;
inline constexpr double kBendCentre = 64.0;
inline constexpr double kSemitoneExponent = 0.08333333333333333;
inline constexpr double kMostRatio = 2.0;
inline constexpr uint32_t kTempoScaleShift = 7;
inline constexpr int kTempoNumerator = 3;
inline constexpr int kTempoShift = 13;
inline constexpr int kTempoBeat = 120;
inline constexpr int kDefaultTempoNumerator = 0x6000;

// libultra's sinf, from the values in the game's code at 0x80056ED0.
inline float Sine(float value)
{
    constexpr std::array<double, 4> kPolynomial = {-0.16666659550427756, 0.008333066246082155, -0.0001980960290193795,
                                                   2.605780637968037e-06};
    constexpr double kInversePi = 0.3183098861837907;
    constexpr double kPiHigh = 3.1415926218032837;
    constexpr double kPiLow = 3.178650954705639e-08;
    constexpr int kExponentOfSmall = 230;
    constexpr int kExponentOfNoReduction = 255;
    constexpr int kExponentOfLargest = 310;

    const auto bits = std::bit_cast<uint32_t>(value);
    const int exponent = static_cast<int>((bits >> 22) & 0x1FF);
    const auto series = [&](double x)
    {
        const double square = x * x;
        const double poly =
            kPolynomial[0] + square * (kPolynomial[1] + square * (kPolynomial[2] + square * kPolynomial[3]));

        return x * square * poly + x;
    };

    if (exponent < kExponentOfNoReduction)
    {
        return exponent >= kExponentOfSmall ? static_cast<float>(series(static_cast<double>(value))) : value;
    }

    if (exponent >= kExponentOfLargest)
    {
        return std::isnan(value) ? value : 0.0F;
    }

    const auto wide = static_cast<double>(value);
    const double scaled = wide * kInversePi;
    const auto turns = static_cast<int32_t>(scaled >= 0.0 ? scaled + 0.5 : scaled - 0.5);
    const auto count = static_cast<double>(turns);
    const double reduced = wide - count * kPiHigh - count * kPiLow;
    const auto result = static_cast<float>(series(reduced));

    return (turns & 1) != 0 ? -result : result;
}

// The game's power of 2, at 0x8003BCE4.
inline float PowerOfTwo(float value)
{
    constexpr std::array<double, 6> kPolynomial = {0.693147180559945,   0.240226506959101,   0.0555041086648216,
                                                   0.00961812910762848, 0.00133335581464284, 0.000154035303933816};
    if (value == 0.0F)
    {
        return 1.0F;
    }

    const float x = value > 0.0F ? value : -value;
    const float square = x * x;
    const float fourth = square * square;
    const double sum = static_cast<double>(x) * kPolynomial[0] + 1.0 + static_cast<double>(square) * kPolynomial[1] +
                       static_cast<double>(square * x) * kPolynomial[2] + static_cast<double>(fourth) * kPolynomial[3] +
                       static_cast<double>(fourth * x) * kPolynomial[4] +
                       static_cast<double>(fourth * square) * kPolynomial[5];

    return static_cast<float>(value > 0.0F ? sum : 1.0 / sum);
}

// The power of 2 of Mario Tennis's build, at 0x802F0958. It's the same series in single precision, with the largest
// power first.
inline float PowerOfTwoInFloats(float value)
{
    if (value == 0.0F)
    {
        return 1.0F;
    }

    const float x = value > 0.0F ? value : -value;
    const float square = value * value;
    const float fourth = square * square;
    float sum = (fourth * square) * std::bit_cast<float>(uint32_t{0x39218489});
    sum += (fourth * x) * std::bit_cast<float>(uint32_t{0x3AAEC3FF});
    sum += fourth * std::bit_cast<float>(uint32_t{0x3C1D955B});
    sum += (square * x) * std::bit_cast<float>(uint32_t{0x3D635847});
    sum += square * std::bit_cast<float>(uint32_t{0x3E75FDF0});
    sum += x * std::bit_cast<float>(uint32_t{0x3F317218});
    sum += 1.0F;

    return value < 0.0F ? 1.0F / sum : sum;
}

// The sine of Mario Tennis, at 0x800385B4, from its table at 0x8005B968.
inline float SineInFloats(float value)
{
    constexpr std::array<uint32_t, 12> kTable = {0x3EA2F983, 0x40490FDB, 0x3F490FDB, 0x3FC90FDB,
                                                 0xBF000000, 0x3D2AAAAB, 0xBAB60B61, 0x37D00D01,
                                                 0xBE2AAAAB, 0x3C088889, 0xB9500D01, 0x3638EF1D};
    const auto table = [&](std::size_t index)
    {
        return std::bit_cast<float>(kTable[index]);
    };

    float x = value;
    const auto turns = static_cast<int32_t>(std::nearbyintf(x * table(0)));
    if (turns != 0)
    {
        const float whole = static_cast<float>(turns) * table(1);
        x = (turns & 1) != 0 ? whole - x : x - whole;
    }

    const float size = std::abs(x);
    float from = x;
    float square = x * x;
    std::size_t row = 8;
    if (table(2) < size)
    {
        const float rest = size - table(3);
        square = rest * rest;
        from = x < 0.0F ? -1.0F : 1.0F;
        row = 4;
    }

    float sum = table(row + 2) + square * table(row + 3);
    sum = table(row + 1) + square * sum;
    sum = table(row) + square * sum;

    return (square * sum) * from + from;
}

// The driver's wave for wave `number` of `bank`, at the places in the model's memory of the samples, the loop's state
// and the book.
inline AlWave DriverWave(const MusSoundBank& bank, int number)
{
    const MusWave& wave = bank.bank->waves[static_cast<std::size_t>(number)];
    AlWave out;
    out.base = bank.sample_address + wave.base;
    out.length = wave.length;
    out.has_loop = wave.has_loop;
    out.loop_start = wave.loop_start;
    out.loop_end = wave.loop_end;
    out.loop_count = wave.loop_count;
    out.loop_state = bank.bank_address + wave.loop_state;
    out.book = bank.bank_address + wave.book;
    out.book_size = wave.book_size;

    return out;
}

// The byte at `place` in the channel's song, or 0 past the end of the song.
inline uint8_t ByteAt(const MusChannel& channel, uint32_t place)
{
    if (channel.song == nullptr || place >= channel.song->bytes.size())
    {
        return 0;
    }

    return channel.song->bytes[place];
}

// A size of one byte, or of two bytes when the first has its top bit set. Moves `place` past it.
inline uint32_t Sized(const MusChannel& channel, uint32_t& place)
{
    const uint8_t first = ByteAt(channel, place++);
    if (first < 0x80)
    {
        return first;
    }

    return (static_cast<uint32_t>(first & 0x7F) << 8) | ByteAt(channel, place++);
}

// Steps the player's random state, and returns the state as a signed fraction of `range`: from about -range / 2 to
// range / 2.
inline int Random(uint32_t& state, int range)
{
    // The code at 0x8003C034 and 0x8008D6E0: eight steps of a shift register.
    constexpr uint32_t kTaps = 0x48000000;
    constexpr uint32_t kOneTap = 0x08000000;
    constexpr int kSteps = 8;
    constexpr float kWordScale = 65536.0F;
    for (int i = 0; i < kSteps; i++)
    {
        const uint32_t tapped = state & kTaps;
        state <<= 1;
        if (tapped == kTaps || tapped == kOneTap)
        {
            state |= 1;
        }
    }

    const float fraction = static_cast<float>(static_cast<int32_t>(state)) / kWordScale / kWordScale;

    return static_cast<int>(static_cast<float>(range) * fraction);
}

// Runs a channel's commands up to its next note. Returns the note's place, or 0 when the channel ends.
// `run_command` runs one command and returns the place after it.
template <typename RunCommand>
uint32_t RunCommands(const MusChannel& channel, const RunCommand& run_command)
{
    uint32_t place = channel.pc;
    int commands = 0;
    while (place != 0)
    {
        const uint8_t command = ByteAt(channel, place);
        if (command < 0x80)
        {
            break;
        }

        place = run_command(command, place + 1);
        commands++;
        if (commands > kMostCommandsInAStep)
        {
            place = 0;
        }
    }

    return place;
}

inline void ReadVolume(MusChannel& channel)
{
    // The code at 0x8003BA8C and 0x8008D1C4.
    while (channel.volume_time < channel.time)
    {
        channel.volume_time += kTick;
        channel.volume_repeat--;
        if (channel.volume_repeat != 0)
        {
            continue;
        }

        const uint8_t value = ByteAt(channel, channel.volume_list++);
        if (value < 0x80)
        {
            channel.volume = value;
            channel.volume_repeat = 1;
            continue;
        }

        channel.volume = value & 0x7F;
        channel.volume_repeat = static_cast<uint16_t>(Sized(channel, channel.volume_list) + 2);
    }
}

inline void ReadBend(MusChannel& channel)
{
    // The code at 0x8003BB84 and 0x8008D288.
    while (channel.bend_time < channel.time)
    {
        channel.bend_time += kTick;
        channel.bend_repeat--;
        if (channel.bend_repeat != 0)
        {
            continue;
        }

        const uint8_t value = ByteAt(channel, channel.bend_list++);
        const auto bend = static_cast<float>(static_cast<double>(static_cast<float>(value & 0x7F)) - kBendCentre);
        channel.bend_value = bend;
        channel.bend = bend * channel.bend_range;
        channel.bend_repeat = value < 0x80 ? uint16_t{1} : static_cast<uint16_t>(Sized(channel, channel.bend_list) + 2);
    }
}

inline float Wobble(MusChannel& channel)
{
    // The code at 0x8003B994 and 0x8008D0FC.
    channel.wobble_count--;
    if (channel.wobble_count == 0)
    {
        if (channel.wobble_value == 0)
        {
            channel.wobble_value = channel.wobble_amount;
            channel.wobble_count = channel.wobble_on;
        }
        else
        {
            channel.wobble_value = 0;
            channel.wobble_count = channel.wobble_off;
        }
    }

    return static_cast<float>(channel.wobble_value);
}

// The command "next" (0x96): the end of a pass of the innermost loop that "for" opened. A loop with a count of 0xFF
// repeats for good. The last pass closes the loop, and the channel goes on at `place`. Any other pass takes the lists
// back to where the loop opened, and the channel goes on at the loop's start. The early form also takes the bend range
// back to its default when it does.
inline uint32_t RunNext(MusChannel& channel, uint32_t place, bool resets_bend_range)
{
    const auto depths = static_cast<int>(channel.fors.size());
    const int depth = static_cast<int>(channel.for_depth) - 1;
    auto& open = channel.fors[static_cast<std::size_t>((depth + depths) % depths)];
    if (open.count != kForever)
    {
        open.count--;
        if (open.count == 0)
        {
            channel.for_depth = static_cast<uint8_t>(depth);

            return place;
        }
    }

    channel.volume_list = open.volume_place;
    channel.bend_list = open.bend_place;
    channel.volume = open.volume;
    channel.bend_value = static_cast<float>(open.bend);
    if (resets_bend_range)
    {
        channel.bend_range = kDefaultBendRange;
    }

    channel.bend = channel.bend_value * channel.bend_range;
    channel.volume_repeat = open.volume_repeat;
    channel.bend_repeat = open.bend_repeat;

    return open.place;
}

// Runs a command that both forms of the player run the same way, and returns the place after it. Any other command
// returns 0, and the channel ends. `random` is the player's random state.
inline uint32_t RunCommonCommand(MusChannel& channel, uint8_t command, uint32_t place, uint32_t& random)
{
    const auto byte = [&](uint32_t at)
    {
        return ByteAt(channel, at);
    };

    switch (command)
    {
    case kWave:
        channel.wave_number = static_cast<uint16_t>(Sized(channel, place));

        return place;

    case kPortamento:
        channel.portamento = byte(place);
        if (channel.portamento != 0)
        {
            channel.portamento_now = channel.note_pitch;
        }

        return place + 1;

    case kPortamentoOff:
        channel.portamento = 0;

        return place;

    case kCutoff: // a note's length to its release
        channel.cutoff = static_cast<uint16_t>((uint32_t{byte(place)} << 8) | byte(place + 1));
        channel.end_it = 0;

        return place + 2;

    case kEndIt: // the release's distance from the note's end
        channel.cutoff = 0;
        channel.end_it = byte(place);

        return place + 1;

    case kVibratoOff:
        channel.vibrato_speed = 0;
        channel.vibrato = 0.0F;

        return place;

    case kLength: // a fixed length for notes
        channel.fixed_length = static_cast<uint16_t>(Sized(channel, place));

        return place;

    case kIgnoreLength: // the next note gives a length
        channel.ignore_length = true;

        return place;

    case kTranspose:
        channel.transpose = static_cast<int8_t>(byte(place));

        return place + 1;

    case kIgnoreTranspose: // notes leave out the transpose. The early form clears the flag at the next pitch
        channel.ignore_transpose = true;

        return place;

    case kFor:
        {
            const uint8_t depth = channel.for_depth;
            auto& open = channel.fors[static_cast<std::size_t>(depth) % channel.fors.size()];
            open.count = byte(place);
            open.place = place + 1;
            open.volume_place = channel.volume_list;
            open.bend_place = channel.bend_list;
            open.volume = channel.volume;
            open.bend = static_cast<uint8_t>(static_cast<int32_t>(channel.bend_value));
            open.volume_repeat = channel.volume_repeat;
            open.bend_repeat = channel.bend_repeat;
            channel.for_depth = static_cast<uint8_t>(depth + 1);

            return place + 1;
        }

    case kWobble:
        channel.wobble_amount = static_cast<int8_t>(byte(place));
        channel.wobble_on = byte(place + 1);
        channel.wobble_off = byte(place + 2);

        return place + 3;

    case kWobbleOff:
        channel.wobble_on = 0;

        return place;

    case kVelocityOn: // each note gives a velocity
        channel.velocity_on = true;

        return place;

    case kVelocityOff:
        channel.velocity_on = false;

        return place;

    case kDefaultVelocity:
        channel.velocity_on = false;
        channel.default_velocity = byte(place);

        return place + 1;

    case kPan:
        channel.pan = static_cast<uint8_t>(byte(place) / 2);

        return place + 1;

    case kStereo: // the code at 0x80038844 and 0x8008EBD0 skips it
        return place + 2;

    case kPrint:
        return place + 1;

    case kGoto:
        {
            const uint32_t to = (uint32_t{byte(place)} << 8) | byte(place + 1);
            const uint32_t volume = (uint32_t{byte(place + 2)} << 8) | byte(place + 3);
            const uint32_t bend = (uint32_t{byte(place + 4)} << 8) | byte(place + 5);
            channel.volume_list = channel.volume_base + volume;
            channel.volume_repeat = 1;
            channel.bend_list = channel.bend_base + bend;
            channel.bend_repeat = 1;

            return channel.data_base + to;
        }

    case kReverb:
        channel.reverb = byte(place);

        return place + 1;

    case kRandomNote: // a random transpose
        channel.transpose =
            static_cast<int8_t>(static_cast<int8_t>(Random(random, byte(place))) + static_cast<int>(byte(place + 1)));

        return place + 2;

    case kRandomVolume: // a random volume
        channel.volume = static_cast<uint8_t>(Random(random, byte(place)) + byte(place + 1));

        return place + 2;

    case kRandomPan: // a random pan
        channel.pan = static_cast<uint8_t>(Random(random, byte(place)) + byte(place + 1));

        return place + 2;

    case kVolume:
        channel.volume = byte(place);

        return place + 1;

    case kStartEffect: // the model has no table of effects
        Sized(channel, place);

        return place;

    case kBendRange: // the code at 0x80038B00 and 0x8008EE98 squares the bend, up to the bend list's next value
        channel.bend_range = static_cast<float>(static_cast<double>(static_cast<float>(byte(place))) * kBendRangeUnit);
        channel.bend = channel.bend_value * channel.bend_value;

        return place + 1;

    default:
        break;
    }

    return 0;
}

} // namespace ultralib::n64::mus_player_common
