// SPDX-License-Identifier: MIT

#include "ultralib/sequence_export/n64/wave.h"

#include <algorithm>
#include <cstddef>
#include <utility>
#include <vector>

#include "xsflib/binary.h"

namespace ultralib::sequence_export::n64
{

namespace n64 = ultralib::n64;

seqlib::Pcm DecodeWaveSamples(std::span<const uint8_t> bytes, const n64::VadpcmBook* book, const n64::BankLoop* loop,
                              VadpcmFrames frames)
{
    const bool small = frames == VadpcmFrames::kSmall;
    const std::size_t frame_size = small ? n64::kVadpcmSmallFrameSize : n64::kVadpcmFrameSize;
    const auto decode = [&](std::span<const uint8_t> from, n64::VadpcmState& state)
    {
        return small ? n64::DecodeVadpcmSmall(from, *book, state) : n64::DecodeVadpcm(from, *book, state);
    };

    seqlib::Pcm pcm;
    std::vector<int16_t> samples;
    if (book != nullptr)
    {
        n64::VadpcmState start_state = {};
        samples = decode(bytes, start_state);
    }
    else
    {
        samples.reserve(bytes.size() / 2);
        for (std::size_t i = 0; i + 1 < bytes.size(); i += 2)
        {
            samples.push_back(static_cast<int16_t>(xsflib::LoadBe16(bytes.data() + i)));
        }
    }

    if (loop != nullptr && loop->count != 0 && loop->end > loop->start && loop->end <= samples.size())
    {
        samples.resize(loop->end);
        pcm.loop = true;
        pcm.loop_start = loop->start;
        pcm.loop_end = loop->end;
        const std::size_t first_frame = loop->start / n64::kVadpcmFrameSamples;
        const std::size_t next_place = (first_frame + 1) * frame_size;
        if (book != nullptr && loop->has_state && next_place <= bytes.size())
        {
            n64::VadpcmState state = loop->state;
            const std::vector<int16_t> rest = decode(bytes.subspan(next_place), state);
            std::vector<int16_t> repeat(loop->state.begin(), loop->state.end());
            repeat.insert(repeat.end(), rest.begin(), rest.end());
            const std::size_t skip = loop->start - first_frame * n64::kVadpcmFrameSamples;
            const std::size_t length = loop->end - loop->start;
            if (repeat.size() >= skip + length &&
                !std::equal(samples.begin() + static_cast<std::ptrdiff_t>(loop->start), samples.end(),
                            repeat.begin() + static_cast<std::ptrdiff_t>(skip)))
            {
                samples.insert(samples.end(), repeat.begin() + static_cast<std::ptrdiff_t>(skip),
                               repeat.begin() + static_cast<std::ptrdiff_t>(skip + length));
                pcm.loop_start = loop->end;
                pcm.loop_end = loop->end + static_cast<uint32_t>(length);
            }
        }
    }

    pcm.channels.push_back(std::move(samples));

    return pcm;
}

std::shared_ptr<const Wave> DecodeWave(std::span<const uint8_t> bytes, const n64::VadpcmBook* book,
                                       const n64::BankLoop* loop, std::string name, uint32_t rate)
{
    auto out = std::make_shared<Wave>();
    out->name = std::move(name);
    out->rate = rate;
    out->pcm = DecodeWaveSamples(bytes, book, loop);

    return out;
}

} // namespace ultralib::sequence_export::n64
