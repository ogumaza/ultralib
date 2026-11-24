// SPDX-License-Identifier: MIT

// The audio lists in the model of the middle EAD engine, for EAD's microcode and for the standard one: the commands of
// each update for the voices and the reverbs, and the frame that runs the updates.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "ntdlib/rounding.h"

#include "ead_adpcm_pass.h"
#include "ead_common.h"
#include "ead_constants.h"
#include "ead_lists.h"
#include "ead_middle_constants.h"
#include "ead_reverb.h"
#include "ead_standard_lists.h"
#include "ultralib/n64/ead_middle.h"

namespace ultralib::n64
{

namespace
{

using ead_constants::kFrameSamples;
using ead_constants::kWaveSamples;
using ead_lists::Clear;
using ead_lists::FramesPlace;
using ead_lists::Halve;
using ead_lists::kFlagContinue;
using ead_lists::kFlagInit;
using ead_lists::kFlagLoop;
using ead_lists::kOpAdpcm;
using ead_lists::kOpDuplicate;
using ead_lists::kOpEnvMixer;
using ead_lists::kOpEnvSetup1;
using ead_lists::kOpEnvSetup2;
using ead_lists::kOpInterleave;
using ead_lists::kOpLoadBook;
using ead_lists::kOpSegment;
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
using ead_middle_constants::kOneChannel;
using ead_middle_constants::kTwoChannels;

namespace standard = ead_standard_dmem;

// The places in the RSP's memory that the engine's commands name.
constexpr uint32_t kDmemTemp = 0x000;
constexpr uint32_t kDmemResampled = 0x020;
constexpr uint32_t kDmemDecoded = 0x1A0;
constexpr uint32_t kDmemPanTemp = 0x200;
constexpr uint32_t kDmemCompressedEnd = 0x540;
constexpr uint32_t kDmemLeft = 0x540;
constexpr uint32_t kDmemRight = 0x6C0;
constexpr uint32_t kDmemWetLeft = 0x840;
constexpr uint32_t kDmemWetRight = 0x9C0;

// The bytes of an ADPCM frame.
constexpr int kFrameSize = 9;

// The k-th root of `value`, for k from 9 to 24, by Newton's method from 1.5. The code at 0x800B7378 in Wave Race 64
// computes it this way.
double KthRoot(double value, int k)
{
    constexpr double kClose = 1e-07;
    constexpr int kMostSteps = 64;
    double root = 1.5;
    for (int step = 0; step < kMostSteps; step++)
    {
        const double square = root * root;
        const double fourth = square * square;
        const double eighth = fourth * fourth;
        const int degree = k - 9;
        double power = eighth;
        if ((degree & 1) != 0)
        {
            power *= root;
        }

        if ((degree & 2) != 0)
        {
            power *= square;
        }

        if ((degree & 4) != 0)
        {
            power *= fourth;
        }

        if ((degree & 8) != 0)
        {
            power *= eighth;
        }

        const double next = root - (power * root - value) / (k * power);
        const double change = std::abs(next - root);
        root = next;

        if (change < kClose)
        {
            break;
        }
    }

    return root;
}

} // namespace

void EadMiddlePlayer::StartReverbItem(AudioMemory& memory, Reverb& reverb, int length, int update)
{
    ReverbItem& item = reverb.items[reverb.current_frame][static_cast<std::size_t>(update)];
    if (reverb.downsample != 1 && reverb.frames_to_ignore == 0)
    {
        ead_reverb::ThinSamples(memory, reverb, item);
    }

    ead_reverb::PlaceItem(reverb, item, length);
}

void EadMiddlePlayer::LoadReverbSamples(AudioList& list, const Reverb& reverb, int length, int update)
{
    const ReverbItem& item = reverb.items[reverb.current_frame][static_cast<std::size_t>(update)];

    const auto load = [&list](uint32_t dmem, uint32_t address, uint32_t size)
    {
        Load(list, address, dmem, size);
    };

    const auto resample = [&](uint32_t in, uint32_t out, uint32_t size, int side)
    {
        SetBuffer(list, in, out, size);
        Resample(list, reverb.resample_flags, reverb.downsample_pitch,
                 side == 0 ? reverb.resample_left : reverb.resample_right);
    };

    Clear(list, kDmemWetLeft, kTwoChannels);
    ead_reverb::LoadSamples(reverb, item, length, {kDmemWetLeft, kDmemResampled, kOneChannel}, load, resample);
    Mix(list, kTwoChannels, 0x7FFF, kDmemWetLeft, kDmemLeft);
    Mix(list, kTwoChannels, 0x8000U + reverb.gain, kDmemWetLeft, kDmemWetLeft);
}

void EadMiddlePlayer::SaveReverbSamples(AudioList& list, Reverb& reverb, int update)
{
    const ReverbItem& item = reverb.items[reverb.current_frame][static_cast<std::size_t>(update)];
    if (reverb.downsample == 1)
    {
        const auto save = [&list](uint32_t dmem, uint32_t address, uint32_t size)
        {
            Save(list, dmem, address, size);
        };

        ead_reverb::TransferParts(reverb, item, kDmemWetLeft, kOneChannel, save);
    }
    else
    {
        Save(list, kDmemWetLeft, item.downsample_left, kTwoChannels);
        reverb.resample_flags = 0;
    }
}

void EadMiddlePlayer::BuildUpdate(AudioList& list, AudioMemory& memory, uint32_t output, int length, int update)
{
    const auto row = voices_.size() * static_cast<std::size_t>(update);
    const auto state_of = [&](int voice) -> UpdateState&
    {
        return update_states_[row + static_cast<std::size_t>(voice)];
    };

    Clear(list, kDmemLeft, kTwoChannels);
    for (std::size_t r = 0; r < reverbs_.size(); r++)
    {
        LoadReverbSamples(list, reverbs_[r], length, update);
        for (int i = 0; i < config_.voices; i++)
        {
            UpdateState& state = state_of(i);
            if (state.enabled && state.reverb_index == r)
            {
                list.MarkVoice(voices_[static_cast<std::size_t>(i)].track);
                BuildVoice(list, memory, i, state, length, update);
            }
        }

        list.MarkVoicesEnd();
        SaveReverbSamples(list, reverbs_[r], update);
    }

    for (int i = 0; i < config_.voices; i++)
    {
        UpdateState& state = state_of(i);
        if (state.enabled && state.reverb_index >= reverbs_.size() && BankLoaded(state.sound_bank))
        {
            list.MarkVoice(voices_[static_cast<std::size_t>(i)].track);
            BuildVoice(list, memory, i, state, length, update);
        }
    }

    list.MarkChunkEnd();
    const auto size = static_cast<uint32_t>(length) * 2;
    SetBuffer(list, 0, kDmemTemp, size);
    list.commands.push_back({kOpInterleave << 24, Pair(kDmemLeft, kDmemRight)});
    Save(list, kDmemTemp, output, size * 2);
}

void EadMiddlePlayer::BuildVoice(AudioList& list, AudioMemory& memory, int voice_index, UpdateState& state, int length,
                                 int update)
{
    Voice& voice = voices_[static_cast<std::size_t>(voice_index)];
    uint32_t flags = kFlagContinue;
    if (state.needs_init)
    {
        flags = kFlagInit;
        voice.restart = false;
        voice.position = 0;
        voice.position_fraction = 0;
        voice.current_left = 0;
        voice.current_right = 0;
        voice.previous_delay_right = 0;
        voice.previous_delay_left = 0;
    }

    const uint16_t pitch = state.pitch;
    const uint32_t fixed_length = static_cast<uint32_t>(int32_t{pitch} * length * 2) + voice.position_fraction;
    const auto to_load = static_cast<int32_t>(fixed_length >> 16);
    voice.position_fraction = static_cast<uint16_t>(fixed_length);
    uint32_t before_resampling = 0;
    if (state.synthetic_wave)
    {
        before_resampling = LoadWave(list, state, voice, to_load);
    }
    else
    {
        const std::optional<uint32_t> decoded = DecodeSample(list, memory, voice_index, state, to_load, flags, update);
        if (!decoded)
        {
            return;
        }

        before_resampling = *decoded;
    }

    flags = kFlagContinue;
    if (state.needs_init)
    {
        flags = kFlagInit;
        state.needs_init = false;
    }

    const auto size = static_cast<uint32_t>(length) * 2;
    SetBuffer(list, before_resampling, kDmemTemp, size);
    Resample(list, flags, pitch, voice.resample_state);
    int side = 0;
    if (state.delay_right != 0 || voice.previous_delay_right != 0)
    {
        side = 1;
    }
    else if (state.delay_left != 0 || voice.previous_delay_left != 0)
    {
        side = 2;
    }

    BuildEnvelope(list, state, voice, length, side);
    if (state.headset_effects)
    {
        BuildDelay(list, state, voice, static_cast<int>(size), flags, side);
    }
}

// Appends the commands that load a synthetic wave and copy it for the samples of an update. Returns the place of the
// samples before resampling.
uint32_t EadMiddlePlayer::LoadWave(AudioList& list, const UpdateState& state, Voice& voice, int32_t to_load)
{
    Load(list, state.wave_address, kDmemDecoded, kWaveSamples * 2);
    voice.position &= kWaveSamples - 1;
    const int available = kWaveSamples - voice.position;
    if (available < to_load)
    {
        const int copies = (to_load - available + kWaveSamples - 1) / kWaveSamples;
        if (copies != 0)
        {
            list.commands.push_back({Word0(kOpDuplicate, static_cast<uint32_t>(copies), kDmemDecoded),
                                     Pair(kDmemDecoded + kWaveSamples * 2, kWaveSamples * 2)});
        }
    }

    const uint32_t before_resampling = kDmemDecoded + static_cast<uint32_t>(voice.position) * 2;
    voice.position += to_load;

    return before_resampling;
}

// Appends the commands that decode the ADPCM samples of an update. A two-part update decodes two parts and halves each
// one. Returns the place of the samples before resampling. Returns nothing for a sample with no loop or no book.
std::optional<uint32_t> EadMiddlePlayer::DecodeSample(AudioList& list, AudioMemory& memory, int voice_index,
                                                      UpdateState& state, int32_t to_load, uint32_t flags, int update)
{
    std::optional<SampleDecode> started = StartDecode(memory, state);
    if (!started)
    {
        return std::nullopt;
    }

    SampleDecode& decode = *started;
    const int parts = state.two_parts ? 2 : 1;
    uint32_t loaded_book = 0;
    int skip_bytes = 0;
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

        if (loaded_book != decode.book_address)
        {
            loaded_book = decode.book_address;
            const VadpcmBook& book = decode.bank->books[static_cast<std::size_t>(decode.sample->book)].book;
            const auto entries = static_cast<uint32_t>(kFrameSamples * book.order * book.predictor_count);
            list.commands.push_back({(kOpLoadBook << 24) | entries, decode.book_address + state.book_offset});
        }

        if (state.book_offset != 0)
        {
            // The engine's record of the loaded book then names another table. The next part loads the book again.
            loaded_book = 1;
        }

        while (decode.processed != decode.wanted)
        {
            if (DecodeChunk(list, voice_index, state, decode, flags, skip_bytes, update))
            {
                break;
            }
        }

        const int wanted = decode.wanted;
        if (parts == 1)
        {
            before_resampling = kDmemDecoded + static_cast<uint32_t>(skip_bytes);
        }
        else if (part == 0)
        {
            Halve(list, static_cast<uint32_t>(ntdlib::AlignUp(wanted / 2, 8)),
                  kDmemDecoded + static_cast<uint32_t>(skip_bytes), kDmemResampled);
            resampled_length = wanted;
            before_resampling = kDmemResampled;
            if (state.finished)
            {
                Clear(list, before_resampling + static_cast<uint32_t>(resampled_length),
                      static_cast<uint32_t>(wanted) + 0x10);
            }
        }
        else
        {
            Halve(list, static_cast<uint32_t>(ntdlib::AlignUp(wanted / 2, 8)),
                  kDmemDecoded + static_cast<uint32_t>(skip_bytes),
                  static_cast<uint32_t>(resampled_length) + kDmemResampled);
        }

        if (state.finished)
        {
            break;
        }
    }

    return before_resampling;
}

// Appends the commands that load and decode the next chunk of a part's ADPCM frames. Moves the voice on. Returns
// whether the note has finished.
bool EadMiddlePlayer::DecodeChunk(AudioList& list, int voice_index, UpdateState& state, SampleDecode& decode,
                                  uint32_t& flags, int& skip_bytes, int update)
{
    Voice& voice = voices_[static_cast<std::size_t>(voice_index)];
    const int to_process = decode.wanted - decode.processed;
    const EadAdpcmPass pass = PlanAdpcmPass(voice.position, voice.restart, decode.end_position - voice.position,
                                            to_process, decode.loop->count != 0);
    const uint32_t start_pad = LoadFrames(list, decode.address, voice.position, pass, kFrameSize, kDmemCompressedEnd);
    const uint32_t compressed = FramesPlace(pass, kFrameSize, kDmemCompressedEnd);
    if (voice.restart)
    {
        list.commands.push_back(
            {kOpSetLoop << 24, loop_addresses_[state.sound_bank][static_cast<std::size_t>(decode.sample->loop)]});
        flags = kFlagLoop;
        voice.restart = false;
    }

    const int this_time = pass.Samples();
    const int moved_to = ntdlib::AlignUp(decode.written + kFrameSamples, 16);
    if (decode.processed == 0)
    {
        SetBuffer(list, compressed + start_pad, kDmemDecoded, static_cast<uint32_t>(pass.to_decode) * 2);
        list.commands.push_back({Word0(kOpAdpcm, flags, 0), voice.adpcm_state});
        skip_bytes = pass.ignored_first * 2;
    }
    else
    {
        SetBuffer(list, compressed + start_pad, kDmemDecoded + static_cast<uint32_t>(moved_to),
                  static_cast<uint32_t>(pass.to_decode) * 2);
        list.commands.push_back({Word0(kOpAdpcm, flags, 0), voice.adpcm_state});
        Move(list, kDmemDecoded + static_cast<uint32_t>(moved_to) + static_cast<uint32_t>(pass.ignored_first) * 2,
             kDmemDecoded + static_cast<uint32_t>(decode.written), static_cast<uint32_t>(this_time) * 2);
    }

    decode.processed += this_time;

    if (flags == kFlagInit)
    {
        skip_bytes = kFrameSamples * 2;
        decode.written = pass.to_decode * 2 + kFrameSamples * 2;
    }
    else
    {
        decode.written = WrittenAfterPass(flags == kFlagLoop, decode.written, pass);
    }

    flags = kFlagContinue;

    if (pass.note_finished)
    {
        Clear(list, kDmemDecoded + static_cast<uint32_t>(decode.written),
              static_cast<uint32_t>(decode.wanted - decode.processed) * 2);
        state.finished = true;
        voice.state.finished = true;
        voice.state.enabled = false;
        DisableLaterUpdates(voice_index, update);

        return true;
    }

    AdvancePosition(voice, decode, pass.restart, to_process);

    return false;
}

// Disables a voice in the later updates of the frame, up to the next update that starts it again.
void EadMiddlePlayer::DisableLaterUpdates(int voice_index, int update)
{
    ead_common::DisableLaterUpdates(update_states_, voices_.size(), updates_per_frame_, voice_index, update);
}

// Finds the sample of an update and writes the books and loops of its bank to memory. Returns the sample and its
// places. Returns nothing for a sample with no loop or no book.
std::optional<EadMiddlePlayer::SampleDecode> EadMiddlePlayer::StartDecode(AudioMemory& memory, const UpdateState& state)
{
    const EadSample* sample = SampleOf(state.sound_bank, state.sound);
    if (sample == nullptr || sample->loop == kNoObject || sample->book == kNoObject)
    {
        return std::nullopt;
    }

    EnsureBank(memory, state.sound_bank);
    SampleDecode decode;
    decode.sample = sample;
    decode.bank = &files_->banks[state.sound_bank];
    decode.loop = &decode.bank->loops[static_cast<std::size_t>(sample->loop)];
    decode.end_position = static_cast<int32_t>(decode.loop->end);
    decode.address = AudioMemory::kImageBase + files_->sample_places[state.sound_bank] + sample->address;
    decode.book_address = book_addresses_[state.sound_bank][static_cast<std::size_t>(sample->book)];

    return decode;
}

// Moves a voice past a chunk of `to_process` samples, or back to the loop start when the sample restarts.
void EadMiddlePlayer::AdvancePosition(Voice& voice, const SampleDecode& decode, bool restart, int to_process)
{
    if (restart)
    {
        voice.restart = true;
        voice.position = static_cast<int32_t>(decode.loop->start);
    }
    else
    {
        voice.position += to_process;
    }
}

void EadMiddlePlayer::BuildEnvelope(AudioList& list, const UpdateState& state, Voice& voice, int length, int side)
{
    const uint16_t source_left = voice.current_left;
    const uint16_t source_right = voice.current_right;
    const auto target_left = static_cast<uint16_t>(state.target_left << 4);
    const auto target_right = static_cast<uint16_t>(state.target_right << 4);
    const int groups = length >> 3;
    const auto ramp_left = static_cast<int16_t>((int32_t{target_left} - source_left) / groups);
    const auto ramp_right = static_cast<int16_t>((int32_t{target_right} - source_right) / groups);
    voice.current_left = static_cast<uint16_t>(source_left + ramp_left * groups);
    voice.current_right = static_cast<uint16_t>(source_right + ramp_right * groups);
    uint32_t dry_left = kDmemLeft;
    uint32_t dry_right = kDmemRight;
    if (state.headset_effects)
    {
        Clear(list, kDmemPanTemp, kOneChannel);
        if (side == 1)
        {
            dry_left = kDmemPanTemp;
        }
        else if (side == 2)
        {
            dry_right = kDmemPanTemp;
        }
    }

    list.commands.push_back({Word0(kOpEnvSetup1, state.reverb, ((source_left & 0xFFU) << 8) | (source_right & 0xFFU)),
                             Pair(static_cast<uint16_t>(ramp_left), static_cast<uint16_t>(ramp_right))});
    list.commands.push_back({kOpEnvSetup2 << 24, Pair(source_left, source_right)});
    const uint32_t bits = (uint32_t{state.strong_right} << 1) | uint32_t{state.strong_left};
    list.commands.push_back(
        {(kOpEnvMixer << 24) | ((static_cast<uint32_t>(length) & 0xFF) << 8) | bits,
         ((dry_left >> 4) << 24) | ((dry_right >> 4) << 16) | ((kDmemWetLeft >> 4) << 8) | (kDmemWetRight >> 4)});
}

void EadMiddlePlayer::BuildDelay(AudioList& list, const UpdateState& state, Voice& voice, int size, uint32_t flags,
                                 int side)
{
    const std::optional<ead_common::HeadsetDelay> delay =
        ead_common::ChooseHeadsetDelay(side, kDmemLeft, kDmemRight, state.delay_left, state.delay_right,
                                       voice.previous_delay_left, voice.previous_delay_right);
    if (!delay)
    {
        return;
    }

    const uint32_t shift = delay->shift;
    const uint32_t previous_shift = delay->previous_shift;
    const auto bytes = static_cast<uint32_t>(size);
    const uint32_t shifted = shift + bytes - previous_shift;
    if (flags != kFlagInit)
    {
        // A small change of rate fits a change of the delay.
        const uint32_t pitch = ead_common::HeadsetResamplePitch(bytes, *delay);
        if (previous_shift == 0)
        {
            Move(list, kDmemPanTemp, 0, 8);
            Clear(list, 8, 8);
            Move(list, kDmemPanTemp, 0x10, 0x10);
            Save(list, 0, voice.pan_resample_state, 0x20);
            SetBuffer(list, kDmemPanTemp + 8, 0, shifted);
            Resample(list, 0, pitch, voice.pan_resample_state);
            Move(list, 0, kDmemPanTemp, shifted);
        }
        else
        {
            SetBuffer(list, kDmemPanTemp, 0, shifted);
            Resample(list, 0, pitch, voice.pan_resample_state);
            Load(list, voice.pan_samples, kDmemPanTemp, previous_shift);
            Move(list, 0, kDmemPanTemp + previous_shift, shifted);
        }
    }
    else
    {
        // The delay only moves the samples to the right.
        Move(list, kDmemPanTemp, 0, bytes);
        Move(list, 0, kDmemPanTemp + shift, bytes);
        Clear(list, kDmemPanTemp, shift);
    }

    if (shift != 0)
    {
        Save(list, kDmemPanTemp + bytes, voice.pan_samples, shift);
    }

    Mix(list, static_cast<uint32_t>(ntdlib::AlignUp(size, 32)), 0x7FFF, kDmemPanTemp, delay->target);
}

void EadMiddlePlayer::BuildRampTables()
{
    // The code at 0x800B7450 in Wave Race 64. An envelope's ramp over an update multiplies a volume by a factor in each
    // group of 8 samples. Each table contains a volume's root for an update of a size: 8 samples shorter, the usual
    // size and 8 samples longer.
    constexpr std::size_t kSteps = 1024;
    constexpr int kVolumeStep = 32;
    constexpr double kScale = 65536.0;
    const int groups = samples_per_update_ / 8;
    for (std::size_t table = 0; table < ramp_up_.size(); table++)
    {
        const int k = groups - 1 + static_cast<int>(table);
        ramp_up_[table].resize(kSteps);
        ramp_down_[table].resize(kSteps);
        for (std::size_t i = 0; i < kSteps; i++)
        {
            const int volume = i == 0 ? 1 : static_cast<int>(i) * kVolumeStep;
            ramp_up_[table][i] = static_cast<float>(KthRoot(volume, k));
            ramp_down_[table][i] = static_cast<float>(KthRoot(1.0 / volume, k) * kScale);
        }
    }
}

void EadMiddlePlayer::LoadReverbStandard(AudioList& list, const Reverb& reverb, int length, int update) const
{
    const ReverbItem& item = reverb.items[reverb.current_frame][static_cast<std::size_t>(update)];

    const auto load = [&list](uint32_t dmem, uint32_t address, uint32_t size)
    {
        list.SetBuffer(0, dmem, 0, size);
        list.LoadBuffer(address);
    };

    const auto resample = [&](uint32_t in, uint32_t out, uint32_t size, int side)
    {
        list.SetBuffer(0, in, out, size);
        list.Resample(reverb.resample_flags, reverb.downsample_pitch,
                      side == 0 ? reverb.resample_left : reverb.resample_right);
    };

    list.ClearBuffer(standard::kWetLeft, standard::kTwoChannels);
    ead_reverb::LoadSamples(reverb, item, length, {standard::kWetLeft, standard::kResampled, standard::kOneChannel},
                            load, resample);
    list.SetBuffer(0, 0, 0, standard::kTwoChannels);
    list.Mixer(0, 0x7FFF, standard::kWetLeft, standard::kLeft);
    list.Mixer(0, 0x8000U + reverb.gain, standard::kWetLeft, standard::kWetLeft);
}

void EadMiddlePlayer::SaveReverbStandard(AudioList& list, Reverb& reverb, int update) const
{
    const ReverbItem& item = reverb.items[reverb.current_frame][static_cast<std::size_t>(update)];
    if (reverb.downsample == 1)
    {
        const auto save = [&list](uint32_t dmem, uint32_t address, uint32_t size)
        {
            list.SetBuffer(0, 0, dmem, size);
            list.SaveBuffer(address);
        };

        ead_reverb::TransferParts(reverb, item, standard::kWetLeft, standard::kOneChannel, save);
    }
    else
    {
        list.SetBuffer(0, 0, standard::kWetLeft, standard::kTwoChannels);
        list.SaveBuffer(item.downsample_left);
        reverb.resample_flags = 0;
    }
}

void EadMiddlePlayer::BuildUpdateStandard(AudioList& list, AudioMemory& memory, uint32_t output, int length, int update)
{
    // The code at 0x800B59A8 in Wave Race 64.
    const auto row = voices_.size() * static_cast<std::size_t>(update);
    const auto state_of = [&](int voice) -> UpdateState&
    {
        return update_states_[row + static_cast<std::size_t>(voice)];
    };

    list.ClearBuffer(standard::kLeft, standard::kTwoChannels);
    bool use_reverb = false;
    for (std::size_t r = 0; r < reverbs_.size(); r++)
    {
        use_reverb = config_.reverbs[r].in_use;
        if (use_reverb)
        {
            LoadReverbStandard(list, reverbs_[r], length, update);
        }

        for (int i = 0; i < config_.voices; i++)
        {
            UpdateState& state = state_of(i);
            if (state.enabled && state.reverb_index == r)
            {
                list.MarkVoice(voices_[static_cast<std::size_t>(i)].track);
                standard_reverb_ = use_reverb;
                BuildVoiceStandard(list, memory, i, state, length);
            }
        }

        list.MarkVoicesEnd();
        if (use_reverb)
        {
            SaveReverbStandard(list, reverbs_[r], update);
        }
    }

    for (int i = 0; i < config_.voices; i++)
    {
        UpdateState& state = state_of(i);
        if (state.enabled && state.reverb_index >= reverbs_.size() && BankLoaded(state.sound_bank))
        {
            list.MarkVoice(voices_[static_cast<std::size_t>(i)].track);
            standard_reverb_ = use_reverb;
            BuildVoiceStandard(list, memory, i, state, length);
        }
    }

    list.MarkChunkEnd();
    const auto size = static_cast<uint32_t>(length) * 2;
    list.SetBuffer(0, 0, kDmemTemp, size);
    list.Interleave(standard::kLeft, standard::kRight);
    list.SetBuffer(0, 0, kDmemTemp, size * 2);
    list.SaveBuffer(output);
}

void EadMiddlePlayer::BuildVoiceStandard(AudioList& list, AudioMemory& memory, int voice_index, UpdateState& state,
                                         int length)
{
    // The code at 0x800B5E70 in Wave Race 64.
    Voice& voice = voices_[static_cast<std::size_t>(voice_index)];
    if (!voice.state.enabled)
    {
        return;
    }

    uint8_t flags = kFlagContinue;
    if (state.needs_init)
    {
        flags = kFlagInit;
        voice.restart = false;
        voice.position = 0;
        voice.position_fraction = 0;
        voice.current_left = 1;
        voice.current_right = 1;
        voice.previous_delay_right = 0;
        voice.previous_delay_left = 0;
    }

    const uint16_t pitch = state.pitch;
    const uint32_t fixed_length = static_cast<uint32_t>(int32_t{pitch} * length * 2) + voice.position_fraction;
    const auto to_load = static_cast<int32_t>(fixed_length >> 16);
    voice.position_fraction = static_cast<uint16_t>(fixed_length);
    uint32_t before_resampling = 0;
    if (state.synthetic_wave)
    {
        before_resampling = LoadWaveStandard(list, state, voice, to_load);
    }
    else
    {
        const std::optional<uint32_t> decoded = DecodeSampleStandard(list, memory, voice, state, to_load, flags);
        if (!decoded)
        {
            return;
        }

        before_resampling = *decoded;
    }

    flags = kFlagContinue;
    if (state.needs_init)
    {
        flags = kFlagInit;
        state.needs_init = false;
    }

    const auto size = static_cast<uint32_t>(length) * 2;
    list.SetBuffer(0, before_resampling, kDmemTemp, size);
    list.Resample(flags, pitch, voice.resample_state);
    int side = 0;
    if (state.delay_right != 0 || voice.previous_delay_right != 0)
    {
        side = 1;
    }
    else if (state.delay_left != 0 || voice.previous_delay_left != 0)
    {
        side = 2;
    }

    BuildEnvelopeStandard(list, state, voice, length, side, standard_reverb_);
    if (state.headset_effects)
    {
        BuildDelayStandard(list, state, voice, static_cast<int>(size), flags, side);
    }
}

// LoadWave in the lists for the standard microcode.
uint32_t EadMiddlePlayer::LoadWaveStandard(AudioList& list, const UpdateState& state, Voice& voice, int32_t to_load)
{
    list.SetBuffer(0, standard::kDecoded, 0, kWaveSamples * 2);
    list.LoadBuffer(state.wave_address);
    voice.position &= kWaveSamples - 1;
    const int available = kWaveSamples - voice.position;
    if (available < to_load)
    {
        const int copies = (to_load - available + kWaveSamples - 1) / kWaveSamples;
        for (int i = 0; i < copies; i++)
        {
            list.DmemMove(standard::kDecoded, standard::kDecoded + static_cast<uint32_t>(1 + i) * kWaveSamples * 2,
                          kWaveSamples * 2);
        }
    }

    const uint32_t before_resampling = standard::kDecoded + static_cast<uint32_t>(voice.position) * 2;
    voice.position += to_load;

    return before_resampling;
}

// DecodeSample in the lists for the standard microcode. The list halves the parts by resampling.
std::optional<uint32_t> EadMiddlePlayer::DecodeSampleStandard(AudioList& list, AudioMemory& memory, Voice& voice,
                                                              UpdateState& state, int32_t to_load, uint8_t flags)
{
    std::optional<SampleDecode> started = StartDecode(memory, state);
    if (!started)
    {
        return std::nullopt;
    }

    SampleDecode& decode = *started;
    const int parts = state.two_parts ? 2 : 1;
    uint32_t loaded_book = 0;
    uint32_t skip_bytes = 0;
    uint32_t resampled_length = 0;
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

        if (loaded_book != decode.book_address)
        {
            loaded_book = decode.book_address;
            const VadpcmBook& book = decode.bank->books[static_cast<std::size_t>(decode.sample->book)].book;
            const auto entries = static_cast<uint32_t>(kFrameSamples * book.order * book.predictor_count);
            list.LoadAdpcm(entries, decode.book_address + state.book_offset);
        }

        if (state.book_offset != 0)
        {
            // The engine's record of the loaded book then names another table. The next part loads the book again.
            loaded_book = 1;
        }

        while (decode.processed != decode.wanted)
        {
            if (DecodeChunkStandard(list, voice, state, decode, flags, skip_bytes))
            {
                break;
            }
        }

        if (parts == 1)
        {
            before_resampling = standard::kDecoded + skip_bytes;
        }
        else
        {
            ead_standard_lists::HalvePart(list, part, standard::kDecoded + skip_bytes,
                                          static_cast<uint32_t>(decode.wanted), state.finished, voice.halving_state,
                                          resampled_length);
            before_resampling = standard::kResampled + 4;
        }

        if (state.finished)
        {
            break;
        }
    }

    return before_resampling;
}

// DecodeChunk in the lists for the standard microcode.
bool EadMiddlePlayer::DecodeChunkStandard(AudioList& list, Voice& voice, UpdateState& state, SampleDecode& decode,
                                          uint8_t& flags, uint32_t& skip_bytes)
{
    const int to_process = decode.wanted - decode.processed;
    const EadAdpcmPass pass = PlanAdpcmPass(voice.position, voice.restart, decode.end_position - voice.position,
                                            to_process, decode.loop->count != 0);
    const uint32_t start_pad = ead_standard_lists::LoadFrames(list, decode.address, voice.position, pass);
    if (voice.restart)
    {
        list.SetLoop(loop_addresses_[state.sound_bank][static_cast<std::size_t>(decode.sample->loop)]);
        flags = kFlagLoop;
        voice.restart = false;
    }

    const bool first = decode.processed == 0;
    ead_standard_lists::DecodePass(list, pass, start_pad, first, static_cast<uint32_t>(decode.written), flags,
                                   voice.adpcm_state);
    if (first)
    {
        skip_bytes = static_cast<uint32_t>(pass.ignored_first) * 2;
    }

    decode.processed += pass.Samples();

    if (flags == kFlagInit)
    {
        skip_bytes = 0;
        decode.written = pass.to_decode * 2 + decode.written;
    }
    else
    {
        decode.written = WrittenAfterPass(flags == kFlagLoop, decode.written, pass);
    }

    flags = kFlagContinue;

    if (pass.note_finished)
    {
        list.ClearBuffer(standard::kDecoded + static_cast<uint32_t>(decode.written),
                         static_cast<uint32_t>(decode.wanted - decode.processed) * 2);
        state.finished = true;
        voice.state.finished = true;
        voice.state.enabled = false;

        return true;
    }

    AdvancePosition(voice, decode, pass.restart, to_process);

    return false;
}

void EadMiddlePlayer::BuildEnvelopeStandard(AudioList& list, const UpdateState& state, Voice& voice, int length,
                                            int side, bool use_reverb)
{
    // The code at 0x800B6900 in Wave Race 64.
    const uint16_t source_left = voice.current_left;
    const uint16_t source_right = voice.current_right;
    auto target_left = static_cast<uint16_t>(state.target_left << 5);
    auto target_right = static_cast<uint16_t>(state.target_right << 5);

    if (target_left == 0)
    {
        target_left = 1;
    }

    if (target_right == 0)
    {
        target_right = 1;
    }

    voice.current_left = target_left;
    voice.current_right = target_right;
    const auto size = static_cast<uint32_t>(length) * 2;
    if (state.headset_effects)
    {
        list.ClearBuffer(standard::kPanTemp, standard::kOneChannel);
        list.SetBuffer(0, kDmemTemp, side == 1 ? standard::kPanTemp : standard::kLeft, size);
        list.SetBuffer(kAudioFlagAux, side == 2 ? standard::kPanTemp : standard::kRight, standard::kWetLeft,
                       standard::kWetRight);
    }
    else if (state.strong_right)
    {
        list.ClearBuffer(standard::kStrongDry, standard::kTwoChannels);
        list.SetBuffer(0, kDmemTemp, standard::kStrongDry, size);
        list.SetBuffer(kAudioFlagAux, standard::kRight, standard::kStrongWet, standard::kWetRight);
    }
    else if (state.strong_left)
    {
        list.ClearBuffer(standard::kStrongDry, standard::kTwoChannels);
        list.SetBuffer(0, kDmemTemp, standard::kLeft, size);
        list.SetBuffer(kAudioFlagAux, standard::kStrongDry, standard::kWetLeft, standard::kStrongWet);
    }
    else
    {
        list.SetBuffer(0, kDmemTemp, standard::kLeft, size);
        list.SetBuffer(kAudioFlagAux, standard::kRight, standard::kWetLeft, standard::kWetRight);
    }

    uint8_t flags = kFlagContinue;
    if (target_left != source_left || target_right != source_right || state.mixer_needs_init)
    {
        flags = kFlagInit;
        const std::vector<float>& up = ramp_up_[static_cast<std::size_t>(ramp_table_)];
        const std::vector<float>& down = ramp_down_[static_cast<std::size_t>(ramp_table_)];
        const float ramp_left = up[target_left >> 5U] * down[source_left >> 5U];
        const float ramp_right = up[target_right >> 5U] * down[source_right >> 5U];
        list.SetVolume(kAudioFlagVolume | kAudioFlagLeft, source_left, 0, 0);
        list.SetVolume(kAudioFlagVolume, source_right, 0, 0);
        list.commands.push_back({(uint32_t{kAudioSetVolume} << 24) | (uint32_t{kAudioFlagLeft} << 16) | target_left,
                                 static_cast<uint32_t>(static_cast<int32_t>(ramp_left))});
        list.commands.push_back({(uint32_t{kAudioSetVolume} << 24) | target_right,
                                 static_cast<uint32_t>(static_cast<int32_t>(ramp_right))});
        list.SetVolume(kAudioFlagAux, config_.dry_gain, 0, uint32_t{state.reverb} << 8);
    }

    const bool wet = use_reverb && state.reverb != 0;
    list.EnvMixer(wet ? static_cast<uint8_t>(flags | kAudioFlagAux) : flags, voice.mixer_state);
    if (state.strong_right || state.strong_left)
    {
        constexpr uint32_t kSubtract = 0x8000;
        list.SetBuffer(0, 0, 0, size);
        list.Mixer(0, kSubtract, standard::kStrongDry, state.strong_right ? standard::kLeft : standard::kRight);
        if (wet)
        {
            list.Mixer(0, kSubtract, standard::kStrongWet,
                       state.strong_right ? standard::kWetLeft : standard::kWetRight);
        }
    }
}

void EadMiddlePlayer::BuildDelayStandard(AudioList& list, const UpdateState& state, Voice& voice, int size,
                                         uint32_t flags, int side)
{
    // The code at 0x800B6F18 in Wave Race 64.
    const std::optional<ead_common::HeadsetDelay> delay =
        ead_common::ChooseHeadsetDelay(side, standard::kLeft, standard::kRight, state.delay_left, state.delay_right,
                                       voice.previous_delay_left, voice.previous_delay_right);
    if (delay)
    {
        ead_standard_lists::BuildHeadsetDelay(list, *delay, static_cast<uint32_t>(size), flags == kFlagInit,
                                              voice.pan_resample_state, voice.pan_samples);
    }
}

bool EadMiddlePlayer::Busy() const
{
    return ead_common::AnyEnabled(players_, voices_);
}

void EadMiddlePlayer::RunFrame(AudioList& list, AudioMemory& memory, uint32_t output, int samples)
{
    for (int update = 0; update < updates_per_frame_; update++)
    {
        RunUpdate(update);
    }

    list.commands.push_back({kOpSegment << 24, 0});
    uint32_t address = output;
    int remaining = samples;
    for (int left = updates_per_frame_; left > 0; left--)
    {
        // The size of the update chooses the ramp table.
        const int length = ead_common::UpdateLength(remaining, left, samples_per_update_min_, samples_per_update_,
                                                    samples_per_update_max_, &ramp_table_);
        const int update = updates_per_frame_ - left;
        for (std::size_t r = 0; r < reverbs_.size(); r++)
        {
            if (config_.reverbs[r].in_use)
            {
                StartReverbItem(memory, reverbs_[r], length, update);
            }
        }

        if (config_.standard_microcode)
        {
            BuildUpdateStandard(list, memory, address, length, update);
        }
        else
        {
            BuildUpdate(list, memory, address, length, update);
        }

        remaining -= length;
        address += static_cast<uint32_t>(length) * 4;
    }

    ead_reverb::EndFrame(reverbs_);
}

// Runs the players and the voices for an update. Keeps the state of each voice for the update.
void EadMiddlePlayer::RunUpdate(int update)
{
    for (int player = 0; player < kPlayers; player++)
    {
        if (players_[static_cast<std::size_t>(player)].enabled)
        {
            RunPlayer(player);
            if (players_[static_cast<std::size_t>(player)].enabled)
            {
                PlayerSound(player);
            }
        }
    }

    ProcessVoices();
    const std::size_t row = voices_.size() * static_cast<std::size_t>(update);
    for (std::size_t i = 0; i < voices_.size(); i++)
    {
        Voice& voice = voices_[i];
        if (voice.state.enabled)
        {
            update_states_[row + i] = voice.state;
            voice.state.needs_init = false;
        }
        else
        {
            update_states_[row + i].enabled = false;
        }
    }

    updates_++;
}

} // namespace ultralib::n64
