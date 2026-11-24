// SPDX-License-Identifier: MIT

// The audio lists in the model of the later EAD engine: the commands of each update for the voices and the surround
// sound, and the frame that runs the updates. ead_later_reverb.cpp contains the reverbs' commands.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "ntdlib/rounding.h"

#include "ead_adpcm_pass.h"
#include "ead_common.h"
#include "ead_constants.h"
#include "ead_later_constants.h"
#include "ead_later_tables.h"
#include "ead_lists.h"
#include "ead_reverb.h"
#include "ead_tables.h"
#include "round_toward_zero.h"
#include "ultralib/n64/ead_later.h"

namespace ultralib::n64
{

namespace
{

using ead_constants::kFrameSamples;
using ead_constants::kWaveSamples;
using ead_later_constants::kOneChannel;
using ead_later_constants::kSurroundStateSize;
using ead_later_constants::kTwoChannels;
using ead_lists::Clear;
using ead_lists::Filter;
using ead_lists::FramesPlace;
using ead_lists::Halve;
using ead_lists::kFlagContinue;
using ead_lists::kFlagInit;
using ead_lists::kFlagLoop;
using ead_lists::kOpAddMixer;
using ead_lists::kOpAdpcm;
using ead_lists::kOpDecodeBytes;
using ead_lists::kOpDuplicate;
using ead_lists::kOpEnvMixer;
using ead_lists::kOpEnvSetup1;
using ead_lists::kOpEnvSetup2;
using ead_lists::kOpGain;
using ead_lists::kOpInterleave;
using ead_lists::kOpLoadBook;
using ead_lists::kOpMultiply;
using ead_lists::kOpNegate;
using ead_lists::kOpResampleNearest;
using ead_lists::kOpSetLoop;
using ead_lists::Load;
using ead_lists::LoadFrames;
using ead_lists::Mix;
using ead_lists::Move;
using ead_lists::Pair;
using ead_lists::Resample;
using ead_lists::Save;
using ead_lists::SetBuffer;
using ead_lists::Word0;
using ead_lists::WriteBe16;

namespace tables = ead_later_tables;

constexpr uint32_t kSurroundTemp = 0x4B0;

constexpr uint8_t kCodecNoDecodeSix = 6;
constexpr uint8_t kCodecRawSeven = 7;

} // namespace

// Returns the voices that an update builds, in order: those of each reverb in turn, then the rest. `base` is the
// first record of the update.
std::vector<int> EadLaterPlayer::VoiceOrder(std::size_t base) const
{
    std::vector<int> order;
    order.reserve(voices_.size());
    const int reverb_count = static_cast<int>(reverbs_.size());
    if (reverb_count == 0)
    {
        for (std::size_t i = 0; i < voices_.size(); i++)
        {
            if (update_states_[base + i].enabled)
            {
                order.push_back(static_cast<int>(i));
            }
        }

        return order;
    }

    for (int reverb = 0; reverb < reverb_count; reverb++)
    {
        for (std::size_t i = 0; i < voices_.size(); i++)
        {
            const UpdateState& state = update_states_[base + i];
            if (state.enabled && state.reverb_index == reverb)
            {
                order.push_back(static_cast<int>(i));
            }
        }
    }

    for (std::size_t i = 0; i < voices_.size(); i++)
    {
        const UpdateState& state = update_states_[base + i];
        if (state.enabled && state.reverb_index >= reverb_count)
        {
            order.push_back(static_cast<int>(i));
        }
    }

    return order;
}

void EadLaterPlayer::BuildUpdate(AudioList& list, AudioMemory& memory, uint32_t output, int length, int update)
{
    const std::size_t base = voices_.size() * static_cast<std::size_t>(update);
    const std::vector<int> order = VoiceOrder(base);
    const int reverb_count = static_cast<int>(reverbs_.size());

    Clear(list, dmem_.left, kTwoChannels);
    std::size_t at = 0;
    for (int reverb_index = 0; reverb_index < reverb_count; reverb_index++)
    {
        Reverb& reverb = reverbs_[static_cast<std::size_t>(reverb_index)];
        const bool use_reverb = reverb.enabled;
        if (use_reverb)
        {
            BuildReverbStart(list, reverb, length, update);
        }

        while (at < order.size())
        {
            const int voice = order[at];
            UpdateState& state = update_states_[base + static_cast<std::size_t>(voice)];
            if (state.reverb_index != reverb_index)
            {
                break;
            }

            list.MarkVoice(voices_[static_cast<std::size_t>(voice)].track);
            BuildVoice(list, memory, voice, state, length, update);
            at++;
        }

        list.MarkVoicesEnd();
        if (use_reverb)
        {
            BuildReverbEnd(list, reverb, length, update);
        }
    }

    while (at < order.size())
    {
        const int voice = order[at];
        list.MarkVoice(voices_[static_cast<std::size_t>(voice)].track);
        BuildVoice(list, memory, voice, update_states_[base + static_cast<std::size_t>(voice)], length, update);
        at++;
    }

    list.MarkChunkEnd();
    list.commands.push_back(
        {Word0(kOpInterleave, (static_cast<uint32_t>(length) * 2) >> 4, dmem_.temp), Pair(dmem_.left, dmem_.right)});
    Save(list, dmem_.temp, output, static_cast<uint32_t>(length) * 4);
}

void EadLaterPlayer::BuildWaveLoad(AudioList& list, const UpdateState& state, Voice& voice, int to_load)
{
    if (state.book_offset != 0)
    {
        // Noise: the engine reads on through memory, from the start of wave 7.
        Load(list, waves_address_ + noise_offset_, dmem_.decoded,
             static_cast<uint32_t>(ntdlib::AlignUp(to_load * 2, 16)));
        noise_offset_ += static_cast<uint32_t>(to_load) * 2;

        return;
    }

    Load(list, state.wave_address, dmem_.decoded, kWaveSamples * 2);
    int position = voice.position;
    if (state.harmonics != 0)
    {
        static constexpr std::array<int, 4> kSamplesInPeriod = {kWaveSamples, kWaveSamples / 2, kWaveSamples / 4,
                                                                kWaveSamples / 8};
        position = position * kSamplesInPeriod[state.harmonics >> 2U] / kSamplesInPeriod[state.harmonics & 3U];
    }

    position = static_cast<int>(static_cast<uint32_t>(position) % kWaveSamples);
    const int available = kWaveSamples - position;
    if (to_load > available)
    {
        const int copies = (to_load - available + kWaveSamples - 1) / kWaveSamples;
        if (copies != 0)
        {
            list.commands.push_back({Word0(kOpDuplicate, static_cast<uint32_t>(copies), dmem_.decoded),
                                     Pair(dmem_.decoded + kWaveSamples * 2, 0x80)});
        }
    }

    voice.position = position;
}

// Resets the playback of a voice for a new note.
void EadLaterPlayer::ResetVoicePlayback(Voice& voice, const UpdateState& state)
{
    voice.restart = false;
    voice.position = static_cast<int32_t>(voice.start_position);
    voice.position_fraction = 0;

    if (voice.adsr.start_at_target)
    {
        // The code at 0x8000E810 in Pokémon Stadium 2. A note with a skip point has no ramp from silence.
        voice.current_left = static_cast<int16_t>(state.target_left << 4);
        voice.current_right = static_cast<int16_t>(state.target_right << 4);
    }
    else
    {
        voice.current_left = 0;
        voice.current_right = 0;
    }

    voice.previous_delay_left = 0;
    voice.previous_delay_right = 0;
    voice.reverb = state.reverb;
    voice.parts = 0;
    voice.stop_loop = false;
    voice.comb_needs_init = true;
    voice.state.finished = false;
}

// Adds the command that loads the codebook of an ADPCM sample. Skips it when the codebook is the last one loaded.
void EadLaterPlayer::LoadBook(AudioList& list, const UpdateState& state, const EadSample& sample, const EadBank& bank)
{
    const bool adpcm = sample.codec == kEadCodecAdpcm || sample.codec == kEadCodecSmallAdpcm;
    if (!adpcm || sample.book == kNoObject)
    {
        return;
    }

    const uint32_t book_address = book_addresses_[state.sound_font][static_cast<std::size_t>(sample.book)];
    if (current_book_ == book_address)
    {
        return;
    }

    current_book_ = state.book_offset == 1 ? note_book_address_ + 2 : book_address;
    const VadpcmBook& book = bank.books[static_cast<std::size_t>(sample.book)].book;
    const auto entries = static_cast<uint32_t>(kFrameSamples * book.order * book.predictor_count);
    list.commands.push_back({(kOpLoadBook << 24) | entries, current_book_});
}

// Adds the commands that decode the frames of one pass and move its samples after those of the passes before it.
void EadLaterPlayer::DecodeFrames(AudioList& list, const UpdateState& state, SampleDecode& decode, Voice& voice,
                                  const EadAdpcmPass& pass, int frame_size)
{
    const EadSample& sample = *decode.sample;
    const uint32_t start_pad = LoadFrames(list, decode.address, voice.position, pass, frame_size, dmem_.compressed_end);

    if (voice.restart)
    {
        list.commands.push_back(
            {kOpSetLoop << 24, loop_addresses_[state.sound_font][static_cast<std::size_t>(sample.loop)]});
        decode.flags = kFlagLoop;
        voice.restart = false;
    }

    int moved_to = 0;
    const int this_time = pass.Samples();

    if (decode.processed == 0)
    {
        decode.skip_bytes = pass.ignored_first * 2;
    }
    else
    {
        moved_to = ntdlib::AlignUp(decode.written + 16, 16);
    }

    const uint32_t compressed = FramesPlace(pass, frame_size, dmem_.compressed_end);
    if (sample.codec == kEadCodecAdpcm || sample.codec == kEadCodecSmallAdpcm)
    {
        SetBuffer(list, compressed + start_pad, dmem_.decoded + static_cast<uint32_t>(moved_to),
                  static_cast<uint32_t>(pass.to_decode) * 2);
        const uint32_t codec_flags = sample.codec == kEadCodecSmallAdpcm ? decode.flags | 4U : decode.flags;
        list.commands.push_back({Word0(kOpAdpcm, codec_flags, 0), voice.buffers.adpcm_state});
    }
    else if (sample.codec == kEadCodecS8)
    {
        SetBuffer(list, compressed + start_pad, dmem_.decoded + static_cast<uint32_t>(moved_to),
                  static_cast<uint32_t>(pass.to_decode) * 2);
        list.commands.push_back({Word0(kOpDecodeBytes, decode.flags, 0), voice.buffers.adpcm_state});
    }

    if (decode.processed != 0)
    {
        Move(list, dmem_.decoded + static_cast<uint32_t>(moved_to) + static_cast<uint32_t>(pass.ignored_first) * 2,
             dmem_.decoded + static_cast<uint32_t>(decode.written), static_cast<uint32_t>(this_time) * 2);
    }

    decode.processed += this_time;

    if (decode.flags == kFlagInit)
    {
        decode.skip_bytes = kFrameSamples * 2;
        decode.written = (pass.to_decode + kFrameSamples) * 2;
    }
    else
    {
        decode.written = WrittenAfterPass(decode.flags == kFlagLoop, decode.written, pass);
    }

    decode.flags = kFlagContinue;
}

// Turns off the records of a voice in the later updates of the frame, up to the next note that starts on it.
void EadLaterPlayer::DisableLaterUpdates(int voice_index, int update)
{
    ead_common::DisableLaterUpdates(update_states_, voices_.size(), updates_per_frame_, voice_index, update);
}

// Adds the commands that decode one part of the sample of a voice, a pass at a time. A pass ends at the end of the
// sample or of its loop.
void EadLaterPlayer::DecodePart(AudioList& list, int voice_index, const UpdateState& state, SampleDecode& decode,
                                int update)
{
    Voice& voice = voices_[static_cast<std::size_t>(voice_index)];
    const EadSample& sample = *decode.sample;
    while (decode.processed != decode.wanted)
    {
        const int to_process = decode.wanted - decode.processed;
        EadAdpcmPass pass = PlanAdpcmPass(voice.position, voice.restart, decode.end_position - voice.position,
                                          to_process, decode.loop->count != 0 && !decode.ends_early);
        int frame_size = 9;
        bool decodes = true;
        if (sample.codec == kEadCodecSmallAdpcm)
        {
            frame_size = 5;
        }
        else if (sample.codec == kEadCodecS8)
        {
            frame_size = 16;
        }
        else if (majora_ && sample.codec == kCodecRawSeven)
        {
            frame_size = 4;
        }
        else if (majora_ && sample.codec == kEadCodecReverb)
        {
            pass.note_finished = true;
            decodes = false;
        }
        else if (sample.codec == kEadCodecS16 || sample.codec == kEadCodecS16InMemory ||
                 (majora_ && sample.codec == kCodecNoDecodeSix))
        {
            Clear(list, dmem_.decoded, static_cast<uint32_t>(decode.wanted + kFrameSamples) * 2);
            decode.flags = kFlagContinue;
            decode.skip_bytes = 0;
            decode.processed = decode.wanted;
            decode.written = decode.wanted;
            decodes = false;
        }

        if (decodes)
        {
            DecodeFrames(list, state, decode, voice, pass, frame_size);
        }

        if (pass.note_finished)
        {
            if (!majora_ || stadium_2_ || decode.wanted != decode.processed)
            {
                Clear(list, dmem_.decoded + static_cast<uint32_t>(decode.written),
                      static_cast<uint32_t>(decode.wanted - decode.processed) * 2);
            }

            decode.finished = true;
            voice.state.finished = true;
            DisableLaterUpdates(voice_index, update);
            break;
        }

        if (pass.restart)
        {
            voice.restart = true;
            voice.position = static_cast<int32_t>(decode.loop->start);
        }
        else
        {
            voice.position += to_process;
        }
    }
}

// Adds the commands that load and decode the sample of a voice for an update. Returns the place of the decoded
// samples in DMEM, or nothing for a sound without a sample or a loop.
std::optional<uint32_t> EadLaterPlayer::BuildSampleDecode(AudioList& list, AudioMemory& memory, int voice_index,
                                                          const UpdateState& state, uint32_t flags, bool finished,
                                                          int to_load, int update)
{
    Voice& voice = voices_[static_cast<std::size_t>(voice_index)];
    const EadSample* sample = SampleOf(state.sound_font, state.sound);
    if (sample == nullptr || sample->loop == kNoObject)
    {
        return std::nullopt;
    }

    EnsureBank(memory, state.sound_font);
    const EadBank& bank = files_->banks[state.sound_font];
    const BankLoop& loop = bank.loops[static_cast<std::size_t>(sample->loop)];

    if (majora_ && voice.release_kind != 0)
    {
        voice.stop_loop = true;
    }

    SampleDecode decode;
    decode.sample = sample;
    decode.loop = &loop;
    decode.ends_early = majora_ && loop.count == 2 && voice.stop_loop;
    decode.end_position = static_cast<int32_t>(decode.ends_early ? loop.sample_end : loop.end);
    const uint32_t place = sample->medium < 2 ? files_->sample_places[state.sound_font][sample->medium] : 0;
    decode.address = AudioMemory::kImageBase + place + sample->address;
    decode.flags = flags;
    decode.finished = finished;

    const int parts = state.two_parts ? 2 : 1;
    int resampled_length = 0;
    uint32_t before_resampling = 0;
    for (int part = 0; part < parts; part++)
    {
        decode.processed = 0;
        decode.written = 0;
        decode.wanted = to_load;
        if (parts != 1 && (to_load & 1) != 0)
        {
            decode.wanted = (to_load & ~1) + part * 2;
        }

        LoadBook(list, state, *sample, bank);
        DecodePart(list, voice_index, state, decode, update);
        if (parts == 1)
        {
            before_resampling = dmem_.decoded + static_cast<uint32_t>(decode.skip_bytes);
        }
        else if (part == 0)
        {
            Halve(list, static_cast<uint32_t>(ntdlib::AlignUp(decode.wanted / 2, 8)),
                  dmem_.decoded + static_cast<uint32_t>(decode.skip_bytes), dmem_.temp + kFrameSamples * 2);
            resampled_length = decode.wanted;
            before_resampling = dmem_.temp + kFrameSamples * 2;
            if (decode.finished)
            {
                Clear(list, before_resampling + static_cast<uint32_t>(resampled_length),
                      static_cast<uint32_t>(decode.wanted + kFrameSamples));
            }
        }
        else
        {
            Halve(list, static_cast<uint32_t>(ntdlib::AlignUp(decode.wanted / 2, 8)),
                  dmem_.decoded + static_cast<uint32_t>(decode.skip_bytes),
                  dmem_.temp + kFrameSamples * 2 + static_cast<uint32_t>(resampled_length));
        }

        if (decode.finished)
        {
            break;
        }
    }

    return before_resampling;
}

// Adds the commands that resample the decoded samples of a voice, then those of its book offset and its gain.
void EadLaterPlayer::BuildResample(AudioList& list, const UpdateState& state, const Voice& voice,
                                   uint32_t before_resampling, int length, uint32_t flags)
{
    const uint32_t bytes = static_cast<uint32_t>(length) * 2;
    if (state.pitch == 0)
    {
        Clear(list, dmem_.temp, bytes);
    }
    else
    {
        SetBuffer(list, before_resampling, dmem_.temp, bytes);
        Resample(list, flags, state.pitch, voice.buffers.resample_state);
    }

    if (state.book_offset == 3)
    {
        list.commands.push_back({Word0(kOpNegate, 0, bytes), Pair(dmem_.temp, dmem_.temp)});
    }

    if (state.book_offset == 2)
    {
        list.commands.push_back({Word0(kOpMultiply, 0, bytes), Pair(dmem_.temp, dmem_.temp)});
    }

    int gain = state.gain;
    if (gain != 0)
    {
        if (gain < 0x10)
        {
            gain = 0x10;
        }

        list.commands.push_back(
            {Word0(kOpGain, static_cast<uint32_t>(gain), static_cast<uint32_t>(length + kFrameSamples) * 2),
             Pair(dmem_.temp, 0)});
    }
}

// Adds the commands of the filter and the comb filter of a voice.
void EadLaterPlayer::BuildVoiceFilters(AudioList& list, AudioMemory& memory, const UpdateState& state, Voice& voice,
                                       int length, uint32_t flags)
{
    const uint32_t bytes = static_cast<uint32_t>(length) * 2;
    if (state.filter)
    {
        const std::array<int16_t, 8> taps = FilterTaps(voice, *state.filter);
        for (std::size_t i = 0; i < taps.size(); i++)
        {
            WriteBe16(memory, voice.buffers.filter_taps + static_cast<uint32_t>(i) * 2, taps[i]);
        }

        Filter(list, 2, bytes, voice.buffers.filter_taps);
        Filter(list, flags, dmem_.temp, voice.buffers.filter_state);
    }

    const uint32_t comb_size = state.comb_size;
    if (comb_size != 0 && state.comb_gain != 0)
    {
        Move(list, dmem_.temp, dmem_.comb_temp, bytes);
        const uint32_t comb_place = dmem_.comb_temp - comb_size;
        if (voice.comb_needs_init)
        {
            Clear(list, comb_place, comb_size);
            voice.comb_needs_init = false;
        }
        else
        {
            Load(list, voice.buffers.comb_state, comb_place, comb_size);
        }

        Save(list, dmem_.temp + bytes - comb_size, voice.buffers.comb_state, comb_size);
        Mix(list, bytes, state.comb_gain, dmem_.comb_temp, comb_place);
        Move(list, comb_place, dmem_.temp, bytes);
    }
    else
    {
        voice.comb_needs_init = true;
    }
}

// Returns the side that a voice delays: 1 for the left, 2 for the right and 0 for neither.
int EadLaterPlayer::DelaySide(const UpdateState& state, const Voice& voice)
{
    if (state.delay_left != 0 || voice.previous_delay_left != 0)
    {
        return 1;
    }

    if (state.delay_right != 0 || voice.previous_delay_right != 0)
    {
        return 2;
    }

    return 0;
}

void EadLaterPlayer::BuildVoice(AudioList& list, AudioMemory& memory, int voice_index, UpdateState& state, int length,
                                int update)
{
    Voice& voice = voices_[static_cast<std::size_t>(voice_index)];
    bool finished = state.finished;
    uint32_t flags = kFlagContinue;
    if (state.needs_init)
    {
        flags = kFlagInit;
        ResetVoicePlayback(voice, state);
        finished = false;
    }

    const uint16_t pitch = state.pitch;
    const int parts = state.two_parts ? 2 : 1;
    const uint32_t fixed_length = static_cast<uint32_t>(int32_t{pitch} * length * 2) + voice.position_fraction;
    const auto to_load = static_cast<int32_t>(fixed_length >> 16);
    voice.position_fraction = static_cast<uint16_t>(fixed_length);
    voice.parts = static_cast<uint8_t>(parts);
    uint32_t before_resampling = 0;
    if (state.synthetic_wave)
    {
        BuildWaveLoad(list, state, voice, to_load);
        before_resampling = dmem_.decoded + static_cast<uint32_t>(voice.position) * 2;
        voice.position += to_load;
    }
    else
    {
        const std::optional<uint32_t> decoded =
            BuildSampleDecode(list, memory, voice_index, state, flags, finished, to_load, update);
        if (!decoded)
        {
            return;
        }

        before_resampling = *decoded;
    }

    flags = kFlagContinue;
    if (state.needs_init)
    {
        state.needs_init = false;
        flags = kFlagInit;
    }

    BuildResample(list, state, voice, before_resampling, length, flags);
    BuildVoiceFilters(list, memory, state, voice, length, flags);
    const int delay_side = DelaySide(state, voice);

    if (majora_ && config_.output == EadLaterOutput::kSurroundEffects)
    {
        state.target_left = static_cast<uint16_t>(state.target_left >> 1);
        state.target_right = static_cast<uint16_t>(state.target_right >> 1);

        if (state.surround != 0xFF)
        {
            BuildSurround(list, state, voice, length, flags);
        }
    }

    BuildEnvelope(list, state, voice, length, delay_side);
    if (state.use_delay)
    {
        if ((flags & kFlagInit) == 0)
        {
            flags = kFlagContinue;
        }

        BuildDelay(list, state, voice, static_cast<int>(static_cast<uint32_t>(length) * 2), flags, delay_side);
    }
}

void EadLaterPlayer::BuildSurround(AudioList& list, UpdateState& state, Voice& voice, int length, uint32_t flags)
{
    const auto bytes = static_cast<uint32_t>(length) * 2;
    Move(list, dmem_.temp, dmem_.delay_temp, bytes);
    const uint16_t dry = voice.surround_gain;
    if (flags == kFlagInit)
    {
        Clear(list, kSurroundTemp, kSurroundStateSize);
        voice.surround_gain = 0;
    }
    else
    {
        Load(list, voice.buffers.surround_state, kSurroundTemp, kSurroundStateSize);
        Mix(list, bytes, dry, kSurroundTemp, dmem_.left);
        Mix(list, bytes, dry ^ 0xFFFFU, kSurroundTemp, dmem_.right);
        const int wet = (dry * voice.reverb) >> 7;
        Mix(list, bytes, static_cast<uint16_t>(wet), kSurroundTemp, dmem_.wet_left);
        Mix(list, bytes, static_cast<uint16_t>(wet ^ 0xFFFF), kSurroundTemp, dmem_.wet_right);
    }

    Save(list, kSurroundTemp + bytes, voice.buffers.surround_state, kSurroundStateSize);
    float decay = static_cast<float>(state.target_left + state.target_right) * (1.0F / 0x2000);
    decay = std::min(decay, 1.0F);
    decay *= ead_tables::kDefaultPanVolume[static_cast<std::size_t>(127 - state.surround)];
    voice.surround_gain = static_cast<uint16_t>((decay * 0x7FFF + static_cast<float>(voice.surround_gain)) / 2);
    Move(list, dmem_.delay_temp, dmem_.temp, bytes);
}

void EadLaterPlayer::BuildEnvelope(AudioList& list, const UpdateState& state, Voice& voice, int length, int delay_side)
{
    const auto current_left = static_cast<uint16_t>(voice.current_left);
    const auto current_right = static_cast<uint16_t>(voice.current_right);
    auto target_left = static_cast<uint16_t>(state.target_left << 4);
    auto target_right = static_cast<uint16_t>(state.target_right << 4);
    if (majora_ && config_.output == EadLaterOutput::kSurroundEffects && state.surround != 0xFF)
    {
        const float scale = ead_tables::kDefaultPanVolume[state.surround];
        target_left = static_cast<uint16_t>(static_cast<float>(target_left) * scale);
        target_right = static_cast<uint16_t>(static_cast<float>(target_right) * scale);
    }

    const int groups = length >> 3;
    const int16_t ramp_left =
        target_left != current_left ? static_cast<int16_t>((int32_t{target_left} - current_left) / groups) : int16_t{0};
    const int16_t ramp_right = target_right != current_right
                                   ? static_cast<int16_t>((int32_t{target_right} - current_right) / groups)
                                   : int16_t{0};
    const int source_reverb = voice.reverb;
    const int wet = source_reverb & 0x7F;
    int16_t ramp_reverb = 0;
    if (source_reverb != state.reverb)
    {
        ramp_reverb = static_cast<int16_t>((((state.reverb & 0x7F) - wet) << 9) / groups);
        voice.reverb = state.reverb;
    }

    voice.current_left = static_cast<int16_t>(current_left + ramp_left * groups);
    voice.current_right = static_cast<int16_t>(current_right + ramp_right * groups);

    uint32_t destinations =
        (dmem_.left >> 4 << 24) | (dmem_.right >> 4 << 16) | (dmem_.wet_left >> 4 << 8) | (dmem_.wet_right >> 4);
    if (state.use_delay)
    {
        Clear(list, dmem_.delay_temp, kOneChannel);
        if (delay_side == 1)
        {
            destinations = (dmem_.delay_temp >> 4 << 24) | (dmem_.right >> 4 << 16) | (dmem_.wet_left >> 4 << 8) |
                           (dmem_.wet_right >> 4);
        }
        else if (delay_side == 2)
        {
            destinations = (dmem_.left >> 4 << 24) | (dmem_.delay_temp >> 4 << 16) | (dmem_.wet_left >> 4 << 8) |
                           (dmem_.wet_right >> 4);
        }
    }

    list.commands.push_back({Word0(kOpEnvSetup1, static_cast<uint32_t>(wet * 2), static_cast<uint16_t>(ramp_reverb)),
                             Pair(static_cast<uint16_t>(ramp_left), static_cast<uint16_t>(ramp_right))});
    list.commands.push_back({kOpEnvSetup2 << 24, Pair(current_left, current_right)});
    const uint32_t bits = ((static_cast<uint32_t>(source_reverb) & 0x80) >> 7 << 4) |
                          (uint32_t{state.stereo_effects} << 3) | (uint32_t{state.headset_effects} << 2) |
                          (uint32_t{state.strong_right} << 1) | uint32_t{state.strong_left};
    list.commands.push_back(
        {(kOpEnvMixer << 24) | ((dmem_.temp >> 4) << 16) | ((static_cast<uint32_t>(length) & 0xFF) << 8) | bits,
         destinations});
}

void EadLaterPlayer::BuildDelay(AudioList& list, const UpdateState& state, Voice& voice, int size, uint32_t flags,
                                int delay_side)
{
    uint32_t destination = 0;
    int delay = 0;
    int previous = 0;
    if (delay_side == 1)
    {
        destination = dmem_.left;
        delay = state.delay_left;
        previous = voice.previous_delay_left;
        voice.previous_delay_right = 0;
        voice.previous_delay_left = state.delay_left;
    }
    else if (delay_side == 2)
    {
        destination = dmem_.right;
        delay = state.delay_right;
        previous = voice.previous_delay_right;
        voice.previous_delay_right = state.delay_right;
        voice.previous_delay_left = 0;
    }
    else
    {
        return;
    }

    const auto usize = static_cast<uint32_t>(size);
    const auto udelay = static_cast<uint32_t>(delay);
    const auto uprevious = static_cast<uint32_t>(previous);
    if (flags != kFlagInit)
    {
        if (delay != previous)
        {
            const uint32_t pitch = static_cast<uint32_t>(((size << 15) / 2 - 1) / ((size + delay - previous - 2) / 2));
            SetBuffer(list, dmem_.delay_temp, dmem_.temp, usize + udelay - uprevious);
            list.commands.push_back({Word0(kOpResampleNearest, 0, pitch), 0});
        }
        else
        {
            Move(list, dmem_.delay_temp, dmem_.temp, usize);
        }

        if (previous != 0)
        {
            Load(list, voice.buffers.delay_state, dmem_.delay_temp,
                 static_cast<uint32_t>(ntdlib::AlignUp(previous, 16)));
            Move(list, dmem_.temp, dmem_.delay_temp + uprevious, usize + udelay - uprevious);
        }
        else
        {
            Move(list, dmem_.temp, dmem_.delay_temp, usize + udelay);
        }
    }
    else
    {
        Move(list, dmem_.delay_temp, dmem_.temp, usize);
        if (!majora_ || delay != 0)
        {
            Clear(list, dmem_.delay_temp, udelay);
        }

        Move(list, dmem_.temp, dmem_.delay_temp + udelay, usize);
    }

    if (delay != 0)
    {
        Save(list, dmem_.delay_temp + usize, voice.buffers.delay_state,
             static_cast<uint32_t>(ntdlib::AlignUp(delay, 16)));
    }

    list.commands.push_back({Word0(kOpAddMixer, static_cast<uint32_t>(ntdlib::AlignUp(size, 64)) >> 4, 0x7FFF),
                             Pair(dmem_.delay_temp, destination)});
}

bool EadLaterPlayer::Busy() const
{
    return ead_common::AnyEnabled(players_, voices_);
}

// For each update of a frame, runs the players and records the settings of each voice.
void EadLaterPlayer::RunFrameUpdates()
{
    for (int left = updates_per_frame_; left > 0; left--)
    {
        // Each update's records take their row, from the frame's first update to its last.
        const int update = updates_per_frame_ - left;
        const std::size_t base = voices_.size() * static_cast<std::size_t>(update);

        for (int player = 0; player < config_.players; player++)
        {
            if (players_[static_cast<std::size_t>(player)].enabled)
            {
                RunPlayer(player);
                PlayerSound(player);
            }
        }

        ProcessVoices(static_cast<int>(base));
        for (std::size_t i = 0; i < voices_.size(); i++)
        {
            Voice& voice = voices_[i];
            if (voice.state.enabled)
            {
                voice.state.needs_init = false;
            }
            else
            {
                update_states_[base + i].enabled = false;
            }

            voice.state.harmonics = 0;
        }

        updates_++;
    }
}

void EadLaterPlayer::RunFrame(AudioList& list, AudioMemory& memory, uint32_t output, int samples)
{
    const RoundTowardZero rounding(stadium_2_);
    frames_++;
    RunSlowLoads();
    RunFrameUpdates();

    uint32_t address = output;
    int remaining = samples;
    current_book_ = 0;

    for (int left = updates_per_frame_; left > 0; left--)
    {
        const int length = ead_common::UpdateLength(remaining, left, samples_per_update_min_, samples_per_update_,
                                                    samples_per_update_max_);
        const int update = updates_per_frame_ - left;
        for (std::size_t i = 0; i < reverbs_.size(); i++)
        {
            if (reverbs_[i].enabled)
            {
                StartReverbItems(memory, length, update, static_cast<int>(i));
            }
        }

        BuildUpdate(list, memory, address, length, update);
        remaining -= length;
        address += static_cast<uint32_t>(length) * 4;
    }

    ead_reverb::EndFrame(reverbs_);

    if (!random_set_)
    {
        random_ = (random_ + static_cast<uint32_t>(frames_)) * 0x9E3779B1U;
    }
}

} // namespace ultralib::n64
