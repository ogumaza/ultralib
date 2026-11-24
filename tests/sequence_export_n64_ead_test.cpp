// SPDX-License-Identifier: MIT

// Unit tests for the conversion of the sequences of Nintendo EAD's engines. The tests build small sound files in
// memory. Their expected values follow from the arithmetic of the engines, worked out by hand.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <numbers>
#include <span>
#include <utility>
#include <vector>

#include "xsflib/testing/check.h"
#include "xsflib/testing/near.h"

#include "sequence_export_n64_testing.h"
#include "ultralib/n64/audio_list.h"
#include "ultralib/n64/audio_memory.h"
#include "ultralib/n64/ead_bank.h"
#include "ultralib/n64/ead_later.h"
#include "ultralib/sequence_export/n64/ead.h"

namespace
{

namespace n64 = ultralib::n64;
namespace midi = seqlib::midi;
using ultralib::sequence_export::n64::Zone;
using ultralib::testing::AllEventsOfKind;
using ultralib::testing::EventsOfKind;
using xsflib::testing::Check;
using xsflib::testing::Near;

// A bank of one instrument and one drum. The envelope rises in 4 updates and holds at `top`.
n64::EadBank SmallEadBank(int16_t top = 32767)
{
    n64::EadEnvelope envelope;
    envelope.points = {{4, top}, {1000, top}, {n64::kEnvelopeHang, 0}};

    n64::EadInstrument instrument;
    instrument.release_rate = 0x20;
    instrument.envelope = 0;
    instrument.normal = {.sample = 0, .tuning = 1.0F};

    n64::EadDrum drum;
    drum.release_rate = 0x20;
    drum.pan = 32;
    drum.sound = {.sample = 0, .tuning = 1.0F};
    drum.envelope = 0;

    n64::EadBank bank;
    bank.envelopes.push_back(envelope);
    bank.samples.emplace_back();
    bank.instruments.push_back(instrument);
    bank.program_instruments = {0};
    bank.drums.push_back(drum);
    bank.drum_notes = {0};

    return bank;
}

// The start of a sequence: channel 0 starts at 0x0D, after a tempo of 120, and the sequence's script ticks on for ever
// in steps of 0x60. The channel selects `program` and starts layer 0 at 0x20. It then waits for 96 ticks and ends.
std::vector<uint8_t> EadSequence(std::span<const uint8_t> channel_settings, std::span<const uint8_t> layer)
{
    std::vector<uint8_t> bytes = {0xD7, 0x00, 0x01, 0x90, 0x00, 0x0D, 0xDD, 0x78, 0xFD, 0x60, 0xFB, 0x00, 0x08};
    bytes.insert(bytes.end(), channel_settings.begin(), channel_settings.end());
    const std::array<uint8_t, 6> start_and_wait = {0x90, 0x00, 0x00, 0xFD, 0x60, 0xFF};
    const std::size_t layer_place = bytes.size() + start_and_wait.size();
    bytes.insert(bytes.end(), start_and_wait.begin(), start_and_wait.end());
    bytes[layer_place - 4] = static_cast<uint8_t>(layer_place);
    bytes.insert(bytes.end(), layer.begin(), layer.end());

    return bytes;
}

ultralib::sequence_export::n64::EadPerformance PlayEad(
    const std::vector<uint8_t>& sequence, const n64::EadBank& bank, n64::EadEngine engine = n64::EadEngine::kEarly,
    ultralib::sequence_export::n64::EadLevels levels = ultralib::sequence_export::n64::EadLevels::kEarly)
{
    const std::map<int, n64::EadBank> banks = {{0, bank}};
    ultralib::sequence_export::n64::EadPerformSettings settings;
    settings.engine = engine;
    settings.levels = levels;
    settings.sequence = sequence;
    settings.bank_set = {0};
    settings.banks = &banks;

    return ultralib::sequence_export::n64::PerformEad(settings, {});
}

void TestEadNote()
{
    // A velocity of 100, a gate of 0x80, and a short note: note 39 for 48 ticks.
    const std::vector<uint8_t> sequence = EadSequence(std::array<uint8_t, 2>{0xC1, 0x00},
                                                      std::array<uint8_t, 7>{0xC1, 100, 0xC9, 0x80, 0x27, 0x30, 0xFF});
    const ultralib::sequence_export::n64::EadPerformance played = PlayEad(sequence, SmallEadBank());

    const std::vector<midi::TrackEvent> ons = EventsOfKind(played.performance, midi::EventKind::kNoteOn);
    const std::vector<midi::TrackEvent> offs = EventsOfKind(played.performance, midi::EventKind::kNoteOff);
    Check(ons.size() == 1 && offs.size() == 1, "one note");
    Check(ons[0].tick == 0 && ons[0].key == 60 && ons[0].value == 100, "note 39 is key 60");

    // The note's gate takes half of its 48 ticks: 0x80 * 48 / 256 = 24 ticks stay after its release.
    Check(offs[0].tick == 24, "the gate ends the note");

    // 120 beats a minute in the engine's frames of 16.713 ms, on a video interface of 59.826 fields a second.
    Check(Near(played.performance.tempo.at(0).bpm, 120.0 * 59.826 * 0.016713, 1e-6), "tempo");
    Check(played.presets.size() == 1 && played.presets[0].program == 0, "one preset");
}

void TestEadLevels()
{
    // A channel volume of 64/127 and a scale of 64/128 act squared in MIDI. The controllers hold their square roots.
    const std::vector<uint8_t> sequence = EadSequence(std::array<uint8_t, 6>{0xC1, 0x00, 0xDF, 64, 0xE0, 64},
                                                      std::array<uint8_t, 7>{0xC1, 100, 0xC9, 0x80, 0x27, 0x30, 0xFF});
    const ultralib::sequence_export::n64::EadPerformance played = PlayEad(sequence, SmallEadBank());
    Check(EventsOfKind(played.performance, midi::EventKind::kVolume).at(0).value == 90, "CC7 holds the root");
    Check(EventsOfKind(played.performance, midi::EventKind::kExpression).at(0).value == 90, "CC11 holds the root");

    // A build that squares the volumes acts as CC7 and CC11 do. The controllers hold the volumes.
    const ultralib::sequence_export::n64::EadPerformance squared = PlayEad(
        sequence, SmallEadBank(), n64::EadEngine::kMiddle, ultralib::sequence_export::n64::EadLevels::kVolumesSquared);
    Check(EventsOfKind(squared.performance, midi::EventKind::kVolume).at(0).value == 64, "CC7 holds the volume");
    Check(EventsOfKind(squared.performance, midi::EventKind::kExpression).at(0).value == 64, "CC11 holds the scale");
}

void TestEadForeverLoop()
{
    // The channel restarts its layer every 48 ticks, and the sequence's script comes round every 96. The state repeats
    // after a tick of set-up. The notes of that tick play at the end of each pass. The loop moves back to tick 0.
    std::vector<uint8_t> sequence = {0xD7, 0x00, 0x01, 0x90, 0x00, 0x0D, 0xDD, 0x78, 0xFD, 0x60, 0xFB, 0x00, 0x08};
    const std::array<uint8_t, 10> channel = {0xC1, 0x00, 0x90, 0x00, 0x18, 0xFD, 0x30, 0xFB, 0x00, 0x0F};
    sequence.insert(sequence.end(), channel.begin(), channel.end());
    sequence.push_back(0x00);
    const std::array<uint8_t, 7> layer = {0xC1, 100, 0xC9, 0x80, 0x27, 0x30, 0xFF};
    sequence.insert(sequence.end(), layer.begin(), layer.end());

    const ultralib::sequence_export::n64::EadPerformance played = PlayEad(sequence, SmallEadBank());
    const ultralib::sequence_export::n64::Performance& performance = played.performance;
    Check(performance.loop.has_value() && performance.loop->start == 0 && performance.loop->end == 96, "the loop");
    Check(performance.end_tick == 96, "the end of the pass");

    const std::vector<midi::TrackEvent> ons = EventsOfKind(performance, midi::EventKind::kNoteOn);
    Check(ons.size() == 2 && ons[0].tick == 0 && ons[1].tick == 48, "two notes in a pass");
}

void TestEadLegato()
{
    // A legato layer keeps its note for the same sound. The second note moves the pitch by 2 semitones.
    const std::vector<uint8_t> sequence =
        EadSequence(std::array<uint8_t, 2>{0xC1, 0x00},
                    std::array<uint8_t, 10>{0xC4, 0xC1, 100, 0xC9, 0x00, 0x27, 0x0A, 0x29, 0x0A, 0xFF});
    const ultralib::sequence_export::n64::EadPerformance played = PlayEad(sequence, SmallEadBank());

    const std::vector<midi::TrackEvent> ons = EventsOfKind(played.performance, midi::EventKind::kNoteOn);
    Check(ons.size() == 1 && ons[0].key == 60, "one note");
    const std::vector<midi::TrackEvent> pitches = EventsOfKind(played.performance, midi::EventKind::kPitch);
    Check(pitches.size() == 1 && pitches[0].tick == 10 && Near(pitches[0].semitones, 2.0, 1e-5), "the pitch moves");
    const std::vector<midi::TrackEvent> offs = EventsOfKind(played.performance, midi::EventKind::kNoteOff);
    Check(offs.size() == 1 && offs[0].tick == 20, "the layer's end ends the note");
}

void TestEadPortamento()
{
    // Mode 1, from note 43 to note 39, over 10 updates. A tick is 14359 / 5760 updates. The sweep ends by tick 5.
    const std::vector<uint8_t> sequence =
        EadSequence(std::array<uint8_t, 2>{0xC1, 0x00},
                    std::array<uint8_t, 11>{0xC7, 0x01, 43, 0x0A, 0xC1, 100, 0xC9, 0x00, 0x27, 0x30, 0xFF});
    const ultralib::sequence_export::n64::EadPerformance played = PlayEad(sequence, SmallEadBank());

    const std::vector<midi::TrackEvent> ons = EventsOfKind(played.performance, midi::EventKind::kNoteOn);
    Check(ons.size() == 1 && ons[0].key == 64, "the note starts at the sweep's first note");
    const std::vector<midi::TrackEvent> pitches = EventsOfKind(played.performance, midi::EventKind::kPitch);
    Check(!pitches.empty() && pitches.back().tick <= 5, "the sweep ends");
    Check(Near(pitches.back().semitones, -4.0, 1e-4), "the sweep ends at the note");
}

void TestEadDrums()
{
    // Drums on program 0x7F. A pan weight of 0 leaves the pan to the drum. The pan goes in the zone. Note 0 is key 21.
    const std::vector<uint8_t> sequence = EadSequence(std::array<uint8_t, 4>{0xC1, 0x7F, 0xDC, 0x00},
                                                      std::array<uint8_t, 7>{0xC1, 100, 0xC9, 0x80, 0x00, 0x30, 0xFF});
    const n64::EadBank bank = SmallEadBank();
    const ultralib::sequence_export::n64::EadPerformance played = PlayEad(sequence, bank);

    const std::vector<midi::TrackEvent> ons = EventsOfKind(played.performance, midi::EventKind::kNoteOn);
    Check(ons.size() == 1 && ons[0].key == 21, "the drum's key");
    Check(played.presets.at(0).program == -1 && played.presets.at(0).drum_pan_weight == 0, "the drums' preset");
    Check(EventsOfKind(played.performance, midi::EventKind::kPan).at(0).value == 64,
          "the controller stays at the centre");

    ultralib::sequence_export::n64::EadPerformSettings settings;
    std::vector<int32_t> samples;
    const std::vector<Zone> zones =
        ultralib::sequence_export::n64::EadZones(played.presets[0], bank, settings, samples);
    Check(zones.size() == 1 && samples.size() == 1 && samples[0] == 0, "one zone");
    Check(zones[0].low_key == 21 && zones[0].high_key == 21 && !zones[0].follows_key, "a drum plays on its key");
    Check(Near(zones[0].pan, -0.5, 1e-6), "a drum's pan of 32 is halfway to the left");
}

void TestEadZones()
{
    ultralib::sequence_export::n64::EadPerformSettings settings;
    ultralib::sequence_export::n64::EadPresetKey key;
    key.program = 0;
    std::vector<int32_t> samples;
    const n64::EadBank bank = SmallEadBank();
    const std::vector<Zone> zones = ultralib::sequence_export::n64::EadZones(key, bank, settings, samples);

    // The resampler stops at 3.99992 times the output's rate: just under two octaves above key 60. Key 84 is past it.
    Check(zones.size() == 2, "a zone below the resampler's limit and one at it");
    Check(zones[0].low_key == 21 && zones[0].high_key == 83 && zones[0].follows_key, "the keys that follow");
    Check(zones[1].low_key == 84 && zones[1].high_key == 127 && !zones[1].follows_key, "the keys at the limit");
    Check(Near(zones[0].tune_cents, 0.0, 1e-6), "a tuning of 1");
    Check(Near(zones[1].tune_cents, 1200.0 * std::log2(3.99992), 1e-6), "the limit's pitch");

    // Four updates at 4 * 59.826 updates a second. The amplitude is the square of the line. A straight rise of the same
    // energy takes the last 0.6 of the time.
    const double rise_ms = 4.0 * 1000.0 / (4.0 * 59.826);
    Check(Near(zones[0].delay_ms, 0.4 * rise_ms, 1e-9) && Near(zones[0].attack_ms, 0.6 * rise_ms, 1e-9), "the rise");
    Check(Near(zones[0].gain, 1.0, 1e-6), "the top of the scale");
    Check(Near(zones[0].decay_ms, 0.0, 1e-6), "the level holds");

    // A release rate of 0x20 takes 0x20 * 24 from 32767 in each update. A fall of 100 dB of the same energy takes
    // 2 ln 10 times as long.
    const double release_ms = 32767.0 / (0x20 * 24.0) * 1000.0 / (4.0 * 59.826) * 2.0 * std::numbers::ln10;
    Check(Near(zones[0].release_ms, release_ms, 1e-6), "the release");
}

void TestEadEnvelopeScales()
{
    // The early engine's envelope is the line itself, and the amplitude squares the line. The next build squares the
    // values first, and the amplitude squares the line too. Its mixer volume at full velocity is 127 * 127 out of
    // 32768. A later one squares the values and takes the line as the amplitude, on a mixer volume of 1 at full
    // velocity.
    ultralib::sequence_export::n64::EadPerformSettings settings;
    ultralib::sequence_export::n64::EadPresetKey key;
    key.program = 0;
    std::vector<int32_t> samples;
    const n64::EadBank bank = SmallEadBank(16384);
    const double half = 16384.0 / 32767.0;
    Check(Near(ultralib::sequence_export::n64::EadZones(key, bank, settings, samples).at(0).gain, half * half, 1e-9),
          "the early levels");
    settings.engine = n64::EadEngine::kMiddle;
    settings.levels = ultralib::sequence_export::n64::EadLevels::kEnvelopeSquared;
    Check(Near(ultralib::sequence_export::n64::EadZones(key, bank, settings, samples).at(0).gain,
               half * half * half * half * 16129.0 / 32768.0, 1e-9),
          "the envelope squared");
    settings.levels = ultralib::sequence_export::n64::EadLevels::kVolumesSquared;
    Check(Near(ultralib::sequence_export::n64::EadZones(key, bank, settings, samples).at(0).gain, half * half, 1e-9),
          "the volumes squared");
}

void TestEadVibrato()
{
    // A depth of 256 out of 4096 and the default rate. The curve peaks at 120 steps of 127 to the octave. A period is
    // 65536 / 0x800 = 32 updates. The LFO takes half the span and the zone's tuning takes the middle.
    ultralib::sequence_export::n64::EadPerformSettings settings;
    ultralib::sequence_export::n64::EadPresetKey key;
    key.program = 0;
    key.vibrato_rate = 0x800;
    key.vibrato_depth = 256;
    std::vector<int32_t> samples;
    const n64::EadBank bank = SmallEadBank();
    const std::vector<Zone> zones = ultralib::sequence_export::n64::EadZones(key, bank, settings, samples);
    const double up = 1200.0 * std::log2(1.0 + (256.0 / 4096.0) * (std::pow(2.0, 120.0 / 127.0) - 1.0));
    const double down = 1200.0 * std::log2(1.0 + (256.0 / 4096.0) * (std::pow(2.0, -120.0 / 127.0) - 1.0));
    Check(zones.at(0).vibrato.has_value(), "a vibrato");
    Check(Near(zones[0].vibrato->hz, 4.0 * 59.826 / 32.0, 1e-9), "the rate");
    Check(Near(zones[0].vibrato->depth, (up - down) / 2.0, 1e-9), "the depth");
    Check(Near(zones[0].tune_cents, (up + down) / 2.0, 1e-9), "the middle of the curve");
}

// A note of the later engine, in the terms that the performer and the model share.
struct LaterNote
{
    double update = 0.0;
    int key = 0;
    int velocity = 0;
    int held = 0;
};

using LaterNotes = std::map<int, std::vector<LaterNote>>;
using LaterPorts = std::array<int, 8>;

constexpr LaterPorts kNoLaterPorts = {-1, -1, -1, -1, -1, -1, -1, -1};
constexpr int kLaterUpdatesPerFrame = 3;
constexpr int kLaterFrames = 600;

n64::EadLaterFiles LaterFiles(std::vector<std::vector<uint8_t>> sequences)
{
    n64::EadLaterFiles files;
    files.bank_sets.assign(sequences.size(), {0});
    files.sequences = std::move(sequences);
    files.banks = {SmallEadBank()};
    files.sample_places = {{0, 0}};

    return files;
}

// A sequence of the later engine. It starts channels 0 and 1, sets a tempo of 120 and runs `player`, then waits for 512
// ticks and ends. Each channel selects program 0, starts one layer with `layer_command`, waits and ends.
std::vector<uint8_t> LaterSequence(std::span<const uint8_t> player, std::span<const uint8_t> first_layer,
                                   std::span<const uint8_t> second_layer, uint8_t layer_command = 0x88)
{
    std::vector<uint8_t> bytes = {0xD7, 0x00, 0x03, 0x90, 0x00, 0x00, 0x91, 0x00, 0x00, 0xDD, 0x78};
    bytes.insert(bytes.end(), player.begin(), player.end());
    const std::array<uint8_t, 4> wait_and_end = {0xFD, 0x82, 0x00, 0xFF};
    bytes.insert(bytes.end(), wait_and_end.begin(), wait_and_end.end());

    const auto add_channel = [&](std::size_t start_place, std::span<const uint8_t> layer)
    {
        constexpr std::size_t kChannelSize = 9;
        const std::size_t place = bytes.size();
        const std::size_t layer_place = place + kChannelSize;
        bytes[start_place] = static_cast<uint8_t>(place >> 8);
        bytes[start_place + 1] = static_cast<uint8_t>(place);
        const std::array<uint8_t, kChannelSize> channel = {
            0xC1, 0x00, layer_command, static_cast<uint8_t>(layer_place >> 8), static_cast<uint8_t>(layer_place), 0xFD,
            0x82, 0x00, 0xFF};
        bytes.insert(bytes.end(), channel.begin(), channel.end());
        bytes.insert(bytes.end(), layer.begin(), layer.end());
    };

    add_channel(4, first_layer);
    add_channel(7, second_layer);

    return bytes;
}

LaterNotes LaterModelNotes(const n64::EadLaterFiles& files, int sequence, const LaterPorts& ports)
{
    constexpr uint32_t kOutputSize = 0x4000;
    constexpr std::size_t kWorkSize = 0x400000;
    constexpr int kSamplesPerFrame = 528;

    const std::vector<uint8_t> image(0x1000, 0);
    n64::AudioMemory memory(image, kWorkSize);
    n64::EadLaterPlayer player(n64::EadLaterConfig{}, files, memory);
    const uint32_t output = memory.Allocate(kOutputSize);

    LaterNotes notes;
    player.on_note = [&](const n64::EadNoteStart& note)
    {
        notes[note.channel].push_back({static_cast<double>(note.update), note.key + 21,
                                       static_cast<int>(std::lround(std::sqrt(note.velocity) * 127.0)),
                                       note.length - note.gate});
    };

    for (std::size_t port = 0; port < ports.size(); port++)
    {
        if (ports[port] != -1)
        {
            player.WritePlayerPort(0, static_cast<int>(port), static_cast<int8_t>(ports[port]));
        }
    }

    player.Start(0, sequence);
    n64::AudioList list;
    for (int frame = 0; frame < kLaterFrames && player.Enabled(0); frame++)
    {
        list.Clear();
        player.RunFrame(list, memory, output, kSamplesPerFrame);
    }

    return notes;
}

LaterNotes LaterPerformedNotes(const n64::EadLaterFiles& files, int sequence, const LaterPorts& ports)
{
    std::map<int, n64::EadBank> banks;
    banks[0] = files.banks[0];
    std::vector<std::span<const uint8_t>> sequences;
    for (const std::vector<uint8_t>& bytes : files.sequences)
    {
        sequences.emplace_back(bytes);
    }

    ultralib::sequence_export::n64::EadPerformSettings settings;
    settings.engine = n64::EadEngine::kLater;
    settings.levels = ultralib::sequence_export::n64::EadLevels::kVolumesSquared;
    settings.clock.updates_per_frame = kLaterUpdatesPerFrame;
    settings.sequence = files.sequences[static_cast<std::size_t>(sequence)];
    settings.bank_set = files.bank_sets[static_cast<std::size_t>(sequence)];
    settings.sequences = sequences;
    settings.bank_sets = files.bank_sets;
    settings.ports = ports;
    settings.banks = &banks;
    const ultralib::sequence_export::n64::EadPerformance played =
        ultralib::sequence_export::n64::PerformEad(settings, {});

    const auto& tempo = played.performance.tempo;
    const double updates_per_second = kLaterUpdatesPerFrame * settings.clock.frame_rate;
    const auto update_of = [&](uint32_t tick)
    {
        double seconds = 0.0;
        for (std::size_t i = 0; i < tempo.size(); i++)
        {
            const uint32_t end = i + 1 < tempo.size() ? tempo[i + 1].tick : tick;
            const uint32_t to = std::min(tick, end);
            if (to > tempo[i].tick)
            {
                seconds += (to - tempo[i].tick) * 60.0 / (tempo[i].bpm * 48.0);
            }
        }

        return seconds * updates_per_second;
    };

    LaterNotes notes;
    for (std::size_t channel = 0; channel < played.performance.tracks.size(); channel++)
    {
        std::map<uint32_t, uint32_t> offs;
        for (const midi::TrackEvent& event : played.performance.tracks[channel])
        {
            if (event.kind == midi::EventKind::kNoteOff)
            {
                offs[event.note] = event.tick;
            }
        }

        for (const midi::TrackEvent& event : played.performance.tracks[channel])
        {
            if (event.kind == midi::EventKind::kNoteOn)
            {
                notes[static_cast<int>(channel)].push_back({update_of(event.tick), event.key, event.value,
                                                            static_cast<int>(offs.at(event.note) - event.tick)});
            }
        }
    }

    return notes;
}

// Compares the performer with the model. The model gives the games' audio lists command for command. No value is
// recorded here. A note may start up to two frames from the model's note, because a tick falls between the frame's
// updates.
void CheckSameAsLaterModel(const n64::EadLaterFiles& files, int sequence, const LaterPorts& ports, const char* what)
{
    constexpr double kMostUpdatesApart = 6.0;

    const LaterNotes model = LaterModelNotes(files, sequence, ports);
    const LaterNotes performed = LaterPerformedNotes(files, sequence, ports);
    Check(!model.empty(), what);
    Check(model.size() == performed.size(), what);
    for (const auto& [channel, notes] : model)
    {
        const std::vector<LaterNote>& others = performed.at(channel);
        Check(notes.size() == others.size(), what);
        for (std::size_t i = 0; i < notes.size(); i++)
        {
            Check(Near(notes[i].update, others[i].update, kMostUpdatesApart), what);
            Check(notes[i].key == others[i].key, what);
            Check(std::abs(notes[i].velocity - others[i].velocity) <= 1, what);
            Check(notes[i].held == others[i].held, what);
        }
    }
}

// A velocity of 100, a gate of 0x80 and two notes.
constexpr std::array<uint8_t, 9> kLaterLayer = {0xC1, 100, 0xC9, 0x80, 0x27, 0x30, 0x2B, 0x18, 0xFF};

void TestLaterNotes()
{
    // After 48 ticks the player's tempo gains 16 * 48.
    const std::array<uint8_t, 4> player = {0xFD, 0x30, 0xDC, 0x10};
    const n64::EadLaterFiles files = LaterFiles({LaterSequence(player, kLaterLayer, kLaterLayer)});
    CheckSameAsLaterModel(files, 0, kNoLaterPorts, "the later engine's notes and tempo");
}

void TestLaterLayerNumbers()
{
    // A layer's number above 3 stands for layer 0.
    const n64::EadLaterFiles files = LaterFiles({LaterSequence({}, kLaterLayer, kLaterLayer, 0x8C)});
    CheckSameAsLaterModel(files, 0, kNoLaterPorts, "a layer's high number");
}

void TestLaterPorts()
{
    // The player reads port 4 and disables channel 1 unless the port holds 0.
    const std::array<uint8_t, 4> player = {0x84, 0xF3, 0x01, 0x41};
    const n64::EadLaterFiles files = LaterFiles({LaterSequence(player, kLaterLayer, kLaterLayer)});
    LaterPorts zero = kNoLaterPorts;
    zero[4] = 0;
    LaterPorts one = kNoLaterPorts;
    one[4] = 1;
    CheckSameAsLaterModel(files, 0, zero, "a port of 0");
    CheckSameAsLaterModel(files, 0, one, "a port of 1");
    Check(LaterPerformedNotes(files, 0, zero).contains(1), "a port of 0 keeps channel 1");
    Check(!LaterPerformedNotes(files, 0, one).contains(1), "a port of 1 disables channel 1");
}

void TestLaterChainedSequence()
{
    // The first sequence starts the second in its place.
    const std::array<uint8_t, 3> chain = {0xC4, 0xFF, 0x01};
    const n64::EadLaterFiles files =
        LaterFiles({LaterSequence(chain, kLaterLayer, kLaterLayer), LaterSequence({}, kLaterLayer, kLaterLayer)});
    CheckSameAsLaterModel(files, 0, kNoLaterPorts, "a chained sequence");
}

// The gain that the first note of a sequence's only channel has in its preset. The channel sets `gain`, and the layer
// starts with `layer_start`.
int LaterSampleGain(uint8_t gain, std::vector<uint8_t> layer_start)
{
    std::vector<uint8_t> bytes = {0xD7, 0x00, 0x01, 0x90, 0x00, 0x0A, 0xFD, 0x82, 0x00, 0xFF};
    const std::vector<uint8_t> channel = {0xED, gain, 0xC1, 0x00, 0x88, 0x00, 0x14, 0x82, 0x00, 0xFF};
    bytes.insert(bytes.end(), channel.begin(), channel.end());
    bytes.insert(bytes.end(), layer_start.begin(), layer_start.end());
    bytes.insert(bytes.end(), kLaterLayer.begin(), kLaterLayer.end());

    const n64::EadLaterFiles files = LaterFiles({bytes});
    std::map<int, n64::EadBank> banks;
    banks[0] = files.banks[0];
    ultralib::sequence_export::n64::EadPerformSettings settings;
    settings.engine = n64::EadEngine::kLaterMajora;
    settings.levels = ultralib::sequence_export::n64::EadLevels::kVolumesSquared;
    settings.clock.updates_per_frame = kLaterUpdatesPerFrame;
    settings.sequence = files.sequences[0];
    settings.bank_set = files.bank_sets[0];
    const std::vector<std::span<const uint8_t>> sequences = {files.sequences[0]};
    settings.sequences = sequences;
    settings.bank_sets = files.bank_sets;
    settings.banks = &banks;

    return ultralib::sequence_export::n64::PerformEad(settings, {}).presets.at(0).sample_gain;
}

void TestLaterChannelGain()
{
    Check(LaterSampleGain(34, {}) == 34, "a channel's gain reaches its preset");
    Check(LaterSampleGain(16, {}) == 0, "a gain of 1 changes nothing");
    Check(LaterSampleGain(8, {}) == 0, "the engine reads a lower gain as 1");

    const std::vector<uint8_t> clear_gain = {0xF0, 0x00, 0x40};
    Check(LaterSampleGain(34, clear_gain) == 0, "a layer can drop its channel's gain");
}

void TestLaterChannelLayers()
{
    // A channel starts two layers, by numbers 0 and 4, that each play two notes.
    std::vector<uint8_t> bytes = {0xD7, 0x00, 0x01, 0x90, 0x00, 0x0A, 0xFD, 0x82, 0x00, 0xFF};
    const std::vector<uint8_t> channel = {0xC1, 0x00, 0x88, 0x00, 0x17, 0x8C, 0x00, 0x20, 0xFD, 0x82, 0x00, 0xFF};
    bytes.insert(bytes.end(), channel.begin(), channel.end());
    bytes.push_back(0xFF);
    bytes.insert(bytes.end(), kLaterLayer.begin(), kLaterLayer.end());
    const std::vector<uint8_t> second = {0xC1, 100, 0xC9, 0x80, 0x30, 0x30, 0x34, 0x18, 0xFF};
    bytes.insert(bytes.end(), second.begin(), second.end());

    const n64::EadLaterFiles files = LaterFiles({bytes});
    std::map<int, n64::EadBank> banks;
    banks[0] = files.banks[0];
    const std::vector<std::span<const uint8_t>> sequences = {files.sequences[0]};
    const auto notes = [&](std::size_t layers)
    {
        ultralib::sequence_export::n64::EadPerformSettings settings;
        settings.engine = n64::EadEngine::kLaterMajora;
        settings.levels = ultralib::sequence_export::n64::EadLevels::kVolumesSquared;
        settings.clock.updates_per_frame = kLaterUpdatesPerFrame;
        settings.channel_layers = layers;
        settings.sequence = files.sequences[0];
        settings.bank_set = files.bank_sets[0];
        settings.sequences = sequences;
        settings.bank_sets = files.bank_sets;
        settings.banks = &banks;

        return AllEventsOfKind(ultralib::sequence_export::n64::PerformEad(settings, {}).performance,
                               midi::EventKind::kNoteOn);
    };

    // Ocarina of Time's and Majora's Mask's channels hold four layers. Number 4 stands for the first.
    const std::vector<midi::TrackEvent> four = notes(4);
    Check(four.size() == 2, "number 4 replaces the first layer of four");

    // Pokémon Stadium 2's code at 0x8001EA54 keeps numbers below 6.
    const std::vector<midi::TrackEvent> six = notes(6);
    Check(six.size() == 4, "number 4 is a layer of its own among six");
}

ultralib::sequence_export::n64::EadPerformance PlayLater(const std::vector<uint8_t>& bytes,
                                                         const ultralib::sequence_export::n64::EadGameCall& call = {})
{
    const n64::EadLaterFiles files = LaterFiles({bytes});
    std::map<int, n64::EadBank> banks;
    banks[0] = files.banks[0];
    const std::vector<std::span<const uint8_t>> sequences = {files.sequences[0]};
    ultralib::sequence_export::n64::EadPerformSettings settings;
    settings.engine = n64::EadEngine::kLater;
    settings.levels = ultralib::sequence_export::n64::EadLevels::kVolumesSquared;
    settings.clock.updates_per_frame = kLaterUpdatesPerFrame;
    settings.sequence = files.sequences[0];
    settings.bank_set = files.bank_sets[0];
    settings.sequences = sequences;
    settings.bank_sets = files.bank_sets;
    settings.banks = &banks;
    settings.call = call;

    return ultralib::sequence_export::n64::PerformEad(settings, {});
}

void TestLaterFilter()
{
    // 0xB0 sets the place of the channel's filter. 0xB3 then loads low-pass row 5 and high-pass row 2 there. The note
    // plays through row 5, and the high-pass filter is left out.
    std::vector<uint8_t> bytes = {0xD7, 0x00, 0x01, 0x90, 0x00, 0x0C, 0xDD, 0x78, 0xFD, 0x82, 0x00, 0xFF};
    const std::vector<uint8_t> channel = {0xC1, 0x00, 0xB0, 0x00, 0x40, 0xB3, 0x52,
                                          0x88, 0x00, 0x1A, 0xFD, 0x82, 0x00, 0xFF};
    bytes.insert(bytes.end(), channel.begin(), channel.end());
    const std::vector<uint8_t> layer = {0xC1, 100, 0xC9, 0x80, 0x27, 0x30, 0xFF};
    bytes.insert(bytes.end(), layer.begin(), layer.end());

    const ultralib::sequence_export::n64::EadPerformance played = PlayLater(bytes);

    Check(played.presets.size() == 1 && played.presets[0].low_pass == 5, "the note plays through the loaded row");
    Check(played.performance.approximations.contains("a channel's high-pass filter left out"), "no high-pass filter");
}

void TestLaterStopOtherChannel()
{
    // Channel 0 stops channel 1 at tick 16 with 0xCD 0x01. Channel 1's note ends there. Channel 0's note ends at its
    // gate, 24 ticks in.
    std::vector<uint8_t> bytes = {0xD7, 0x00, 0x03, 0x90, 0x00, 0x0F, 0x91, 0x00,
                                  0x23, 0xDD, 0x78, 0xFD, 0x82, 0x00, 0xFF};
    const std::vector<uint8_t> first = {0xC1, 0x00, 0x88, 0x00, 0x1C, 0xFD, 0x10, 0xCD, 0x01, 0xFD, 0x82, 0x00, 0xFF};
    const std::vector<uint8_t> short_layer = {0xC1, 100, 0xC9, 0x80, 0x27, 0x30, 0xFF};
    const std::vector<uint8_t> second = {0xC1, 0x00, 0x88, 0x00, 0x2C, 0xFD, 0x82, 0x00, 0xFF};
    const std::vector<uint8_t> long_layer = {0xC1, 100, 0xC9, 0x80, 0x27, 0x81, 0x00, 0xFF};
    for (const std::vector<uint8_t>* part : {&first, &short_layer, &second, &long_layer})
    {
        bytes.insert(bytes.end(), part->begin(), part->end());
    }

    const ultralib::sequence_export::n64::Performance performance = PlayLater(bytes).performance;

    const auto off_of = [&](std::size_t track)
    {
        const auto found = std::ranges::find_if(performance.tracks.at(track), [](const midi::TrackEvent& event)
                                                { return event.kind == midi::EventKind::kNoteOff; });
        return found != performance.tracks.at(track).end() ? static_cast<int>(found->tick) : -1;
    };

    Check(off_of(1) == 16, "the other channel stops");
    Check(off_of(0) == 24, "the channel that stops it plays on");
}

void TestLaterGameCall()
{
    // Channel 0 sets its pan to 0 and its reverb to 0x10, and writes 0x40 to its IO port 5. Layer 0 holds a note with a
    // pan of 127. 96 ticks in, the channel reads IO port 4 of channel 15. Unless that's -1, layer 1 plays a second
    // note. The call of Ganon's Tower's level 1 comes on audio frame 30, about 48 ticks in (Ocarina of Time's code at
    // 0x800C5CD8 and 0x800C5D84). Its pan weight of 0 takes the pan from the layer. The volume of 80 gives the player a
    // scale of 80/127, and a reverb of 0x40 - 80 + 127 for the channel's port. Its cutoff of 0x50 goes to channel 15.
    const std::vector<uint8_t> bytes = {
        0xD7, 0x00, 0x01, 0x90, 0x00, 0x0C, 0xDD, 0x78, 0xFD, 0x82, 0x00, 0xFF,                         // the sequence
        0xC1, 0x00, 0xDD, 0x00, 0xCC, 0x40, 0x75, 0xD4, 0x10, 0x88, 0x00, 0x26, 0xFD, 0x60, 0x4F, 0x04, // channel 0
        0xF9, 0x00, 0x25, 0x89, 0x00, 0x30, 0xFD, 0x81, 0x00, 0xFF,                                     //
        0xC1, 0x64, 0xC9, 0x80, 0xCA, 0x7F, 0x27, 0x82, 0x00, 0xFF,                                     // layer 0
        0xC1, 0x64, 0xC9, 0x80, 0x29, 0x81, 0x00, 0xFF};                                                // layer 1
    const ultralib::sequence_export::n64::EadGameCall level_1 = {
        .frame = 30, .pan_weight = 0, .volume = 80, .io_channel = 15, .io_port = 4, .io_value = 0x50, .reverb_port = 5};

    const ultralib::sequence_export::n64::Performance plain = PlayLater(bytes).performance;
    const ultralib::sequence_export::n64::Performance called = PlayLater(bytes, level_1).performance;

    const std::vector<midi::TrackEvent> reverbs = EventsOfKind(called, midi::EventKind::kReverb);
    const std::vector<midi::TrackEvent> pans = EventsOfKind(called, midi::EventKind::kPan);
    const std::vector<midi::TrackEvent> levels = EventsOfKind(called, midi::EventKind::kExpression);
    Check(!reverbs.empty() && reverbs.front().value == 0x10 && reverbs.back().value == 0x40 - 80 + 127,
          "the call gives the reverb from IO port 5");
    Check(pans.size() >= 2 && pans[0].value == 0 && pans[1].value == 127, "a pan weight of 0 takes the layer's pan");
    Check(levels.size() >= 2 && levels[1].value == 80, "the player's volume scale");
    Check(levels.size() >= 2 && pans.size() >= 2 && levels[1].tick == pans[1].tick && pans[1].tick > 40 &&
              pans[1].tick < 56,
          "the call comes on frame 30");
    Check(EventsOfKind(called, midi::EventKind::kNoteOn).size() == 2, "the call writes IO port 4 of channel 15");
    Check(EventsOfKind(plain, midi::EventKind::kNoteOn).size() == 1, "without the call, the port stays at -1");
    Check(EventsOfKind(plain, midi::EventKind::kReverb).back().value == 0x10, "without the call, the reverb stays");

    // The entrance's level, 7, sets the pan weight and waits 2 game frames, 6 audio frames, for the rest.
    ultralib::sequence_export::n64::EadGameCall level_7 = level_1;
    level_7.volume = 60;
    level_7.volume_delay = 6;
    const ultralib::sequence_export::n64::Performance entrance = PlayLater(bytes, level_7).performance;
    const std::vector<midi::TrackEvent> entrance_pans = EventsOfKind(entrance, midi::EventKind::kPan);
    const std::vector<midi::TrackEvent> entrance_levels = EventsOfKind(entrance, midi::EventKind::kExpression);
    Check(entrance_levels.size() >= 2 && entrance_pans.size() >= 2 && entrance_levels[1].value == 60 &&
              entrance_levels[1].tick > entrance_pans[1].tick,
          "the entrance's volume comes later");
    Check(EventsOfKind(entrance, midi::EventKind::kReverb).back().value == 127, "a reverb past 127 stays at 127");
}

void TestMiddleFadeIn()
{
    // 0xDA 0x01 sets a fade-in of 100 updates, and 0xDB then fades the song in to full volume. At 120 bpm, a tick takes
    // 4 * 3590 / 5760 = 2.493 updates. The fade therefore ends in the 41st tick, at tick 40. The note holds for 128
    // ticks, past the end of the fade.
    std::vector<uint8_t> bytes = {0xD7, 0x00, 0x01, 0x90, 0x00, 0x13, 0xDA, 0x01, 0x00, 0x64,
                                  0xDB, 0x7F, 0xDD, 0x78, 0xFD, 0x60, 0xFB, 0x00, 0x0E};
    const std::vector<uint8_t> channel = {0xC1, 0x00, 0x90, 0x00, 0x1C, 0xFD, 0x81, 0x00, 0xFF};
    bytes.insert(bytes.end(), channel.begin(), channel.end());
    const std::vector<uint8_t> layer = {0xC1, 100, 0xC9, 0x80, 0x27, 0x81, 0x00, 0xFF};
    bytes.insert(bytes.end(), layer.begin(), layer.end());

    const ultralib::sequence_export::n64::EadPerformance played = PlayEad(
        bytes, SmallEadBank(), n64::EadEngine::kMiddle, ultralib::sequence_export::n64::EadLevels::kVolumesSquared);

    const std::vector<midi::TrackEvent> levels = EventsOfKind(played.performance, midi::EventKind::kExpression);
    Check(!levels.empty() && levels.back().value == 127, "the song fades in to full volume");
    Check(!levels.empty() && levels.back().tick == 40, "the fade takes 100 updates");
}

void TestLaterZones()
{
    ultralib::sequence_export::n64::EadPerformSettings settings;
    settings.engine = n64::EadEngine::kLater;
    settings.levels = ultralib::sequence_export::n64::EadLevels::kVolumesSquared;
    settings.clock.updates_per_frame = kLaterUpdatesPerFrame;

    // A release rate of 200 falls for 251 - 200 = 51 frames. A straight line of the amplitude reaches -100 dB at 10/3
    // of ln(10) of its time.
    ultralib::sequence_export::n64::EadPresetKey key;
    key.program = 0;
    key.release_rate = 200;
    std::vector<int32_t> samples;
    n64::EadBank bank = SmallEadBank();
    const double seconds = 51.0 / settings.clock.frame_rate;
    const std::vector<Zone> zones = ultralib::sequence_export::n64::EadZones(key, bank, settings, samples);
    Check(Near(zones.at(0).release_ms, 10.0 / 3.0 * std::numbers::ln10 * seconds * 1000.0, 1.0), "a release's time");

    // The filter's row 5 cuts at 4360 Hz, and its gain at no frequency reaches 1.
    key.low_pass = 5;
    const std::vector<Zone> filtered = ultralib::sequence_export::n64::EadZones(key, bank, settings, samples);
    Check(Near(filtered.at(0).low_pass_hz, 4360.0, 1e-6), "the filter's cutoff");
    Check(Near(filtered.at(0).gain, zones.at(0).gain * 0.854, 1e-9), "the filter's gain");

    // Pokémon Stadium 2's code at 0x8001BF34 reads a delay of -4 as a command that takes no time. Its code at
    // 0x8000E810 starts a note whose list opens that way at its first update's level, with no ramp from silence.
    n64::EadBank skipping = SmallEadBank();
    std::vector<n64::EadEnvelope::Point>& points = skipping.envelopes.at(0).points;
    points.insert(points.begin(), {n64::kEnvelopeSkip, 0});
    settings.envelope_skip = true;
    const std::vector<Zone> skipped = ultralib::sequence_export::n64::EadZones(key, skipping, settings, samples);
    Check(Near(skipped.at(0).gain, filtered.at(0).gain, 1e-9), "an envelope's point that takes no time");
    Check(skipped.at(0).attack_ms < filtered.at(0).attack_ms, "a list that opens with that point rises from a level");

    // Bank 0x17 of the game holds (-4, 0), then (1, 32700): the note starts at its top.
    points.at(1).delay = 1;
    const std::vector<Zone> at_once = ultralib::sequence_export::n64::EadZones(key, skipping, settings, samples);
    Check(Near(at_once.at(0).attack_ms, 0.0, 1e-9), "a first line of one update");

    // The point later in a list leaves the note's start alone.
    n64::EadBank later_skip = SmallEadBank();
    std::vector<n64::EadEnvelope::Point>& later_points = later_skip.envelopes.at(0).points;
    later_points.insert(later_points.begin() + 1, {n64::kEnvelopeSkip, 0});
    const std::vector<Zone> later_zones = ultralib::sequence_export::n64::EadZones(key, later_skip, settings, samples);
    settings.envelope_skip = false;
    Check(Near(later_zones.at(0).attack_ms, filtered.at(0).attack_ms, 1e-9), "the point later in a list");

    // Pokémon Stadium 2's code at 0x8001BC5C reads a vibrato as 65536 / (wave * depth / 4096 + 65536), for a wave of up
    // to 32767 either way. A depth of 1024 gives scales of 1 / (1 + 1/8) and 1 / (1 - 1/8).
    ultralib::sequence_export::n64::EadPresetKey shaking = key;
    shaking.vibrato_rate = 0x400;
    shaking.vibrato_depth = 1024;
    const std::vector<Zone> plain_vibrato = ultralib::sequence_export::n64::EadZones(shaking, bank, settings, samples);
    settings.inverse_vibrato = true;
    const std::vector<Zone> inverse_vibrato =
        ultralib::sequence_export::n64::EadZones(shaking, bank, settings, samples);
    settings.inverse_vibrato = false;
    const double up = -1200.0 * std::log2(1.0 - 1.0 / 8.0);
    const double down = -1200.0 * std::log2(1.0 + 1.0 / 8.0);
    Check(plain_vibrato.at(0).vibrato.has_value() &&
              Near(plain_vibrato.at(0).vibrato->depth, 1200.0 * std::log2(1.25), 1e-6),
          "the depth of the usual vibrato");
    Check(inverse_vibrato.at(0).vibrato.has_value() &&
              Near(inverse_vibrato.at(0).vibrato->depth, (up - down) / 2.0, 1e-6),
          "the depth of Pokémon Stadium 2's vibrato");
    Check(Near(inverse_vibrato.at(0).tune_cents - plain_vibrato.at(0).tune_cents, (up + down) / 2.0, 1e-6),
          "the middle of Pokémon Stadium 2's vibrato");
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("sequence_export_n64_ead",
                                    []()
                                    {
                                        TestEadNote();
                                        TestEadLevels();
                                        TestEadForeverLoop();
                                        TestEadLegato();
                                        TestEadPortamento();
                                        TestEadDrums();
                                        TestEadZones();
                                        TestEadEnvelopeScales();
                                        TestEadVibrato();
                                        TestLaterNotes();
                                        TestLaterLayerNumbers();
                                        TestLaterPorts();
                                        TestLaterChainedSequence();
                                        TestLaterChannelGain();
                                        TestLaterChannelLayers();
                                        TestLaterFilter();
                                        TestLaterStopOtherChannel();
                                        TestLaterGameCall();
                                        TestMiddleFadeIn();
                                        TestLaterZones();
                                    });
}
