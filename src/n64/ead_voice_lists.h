// SPDX-License-Identifier: MIT

// The lists of voices that the three EAD engines share. A list is a ring of voice indices in two arrays of links,
// `next` and `previous`. The heads of the lists come after the voices: each pool has four heads, one for each kind of
// list. A voice that is in no list has a previous link of -1. `Voice` is an engine's voice, with its `pool` and its
// `priority`.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "ultralib/n64/ead_list_kind.h"

namespace ultralib::n64::ead_voice_lists
{

inline constexpr int kNoVoice = -1;

// The kinds of list in each pool, in the order of their heads. The loops over a pool's lists take them in this order.
inline constexpr std::array kKinds = {EadListKind::kDisabled, EadListKind::kDecaying, EadListKind::kReleasing,
                                      EadListKind::kActive};

static_assert(static_cast<int>(EadListKind::kDisabled) == 0 && static_cast<int>(EadListKind::kDecaying) == 1 &&
                  static_cast<int>(EadListKind::kReleasing) == 2 && static_cast<int>(EadListKind::kActive) == 3,
              "the heads of a pool's lists follow the order of EadListKind");

// The rule of the search for a voice to take from a list. The early engine takes a voice of lower or equal priority to
// the new note. The middle and later engines take only a voice of lower priority.
enum class PriorityRule : uint8_t
{
    kLowerOrEqual,
    kLower,
};

inline int Head(int voice_count, int pool, EadListKind kind)
{
    return voice_count + pool * static_cast<int>(kKinds.size()) + static_cast<int>(kind);
}

// Pool 0 is the global pool. The players' pools follow it, then the channels' pools.
inline int PlayerPool(int player)
{
    return 1 + player;
}

inline int ChannelPool(int players, int channel)
{
    return 1 + players + channel;
}

// Adds a voice at the end of a list, unless it is in a list already.
template <typename Voice>
void PushBack(std::vector<int>& next, std::vector<int>& previous, std::vector<Voice>& voices, int head, int voice)
{
    const auto place = static_cast<std::size_t>(voice);
    if (previous[place] != kNoVoice)
    {
        return;
    }

    const int last = previous[static_cast<std::size_t>(head)];
    next[static_cast<std::size_t>(last)] = voice;
    previous[place] = last;
    next[place] = head;
    previous[static_cast<std::size_t>(head)] = voice;
    voices[place].pool = (head - static_cast<int>(voices.size())) / static_cast<int>(kKinds.size());
}

// Adds a voice at the start of a list, unless it is in a list already.
template <typename Voice>
void PushFront(std::vector<int>& next, std::vector<int>& previous, std::vector<Voice>& voices, int head, int voice)
{
    const auto place = static_cast<std::size_t>(voice);
    if (previous[place] != kNoVoice)
    {
        return;
    }

    const int first = next[static_cast<std::size_t>(head)];
    previous[place] = head;
    next[place] = first;
    previous[static_cast<std::size_t>(first)] = voice;
    next[static_cast<std::size_t>(head)] = voice;
    voices[place].pool = (head - static_cast<int>(voices.size())) / static_cast<int>(kKinds.size());
}

// Takes a voice out of its list. A voice in no list stays as it is.
inline void Remove(std::vector<int>& next, std::vector<int>& previous, int voice)
{
    const auto place = static_cast<std::size_t>(voice);
    if (previous[place] == kNoVoice)
    {
        return;
    }

    next[static_cast<std::size_t>(previous[place])] = next[place];
    previous[static_cast<std::size_t>(next[place])] = previous[place];
    previous[place] = kNoVoice;
}

// Takes the last voice out of a list and returns it, or kNoVoice when the list is empty.
inline int PopBack(std::vector<int>& next, std::vector<int>& previous, int head)
{
    const int last = previous[static_cast<std::size_t>(head)];
    if (last == head)
    {
        return kNoVoice;
    }

    Remove(next, previous, last);

    return last;
}

// Moves every voice of a pool's lists to the end of the same lists of the global pool. The count of moves stops at the
// number of voices. A broken list then can't loop.
template <typename Voice>
void ClearPool(std::vector<int>& next, std::vector<int>& previous, std::vector<Voice>& voices, int pool)
{
    const auto voice_count = static_cast<int>(voices.size());
    for (const EadListKind kind : kKinds)
    {
        const int source = Head(voice_count, pool, kind);
        const int target = Head(voice_count, 0, kind);
        for (int moved = 0; moved <= voice_count; moved++)
        {
            const int voice = next[static_cast<std::size_t>(source)];
            if (voice == source)
            {
                break;
            }

            Remove(next, previous, voice);
            PushBack(next, previous, voices, target, voice);
        }
    }
}

// Empties a pool, then moves `count` voices into it from the global pool: from the ends of the disabled, decaying,
// releasing and active lists, in that order.
template <typename Voice>
void FillPool(std::vector<int>& next, std::vector<int>& previous, std::vector<Voice>& voices, int pool, int count)
{
    ClearPool(next, previous, voices, pool);
    const auto voice_count = static_cast<int>(voices.size());
    int taken = 0;
    for (const EadListKind kind : kKinds)
    {
        while (taken < count)
        {
            const int voice = PopBack(next, previous, Head(voice_count, 0, kind));
            if (voice == kNoVoice)
            {
                break;
            }

            PushBack(next, previous, voices, Head(voice_count, pool, kind), voice);
            taken++;
        }
    }
}

// Finds the voice of the lowest priority in a list: the last of them in the list's order. Returns kNoVoice when the
// list is empty, or when that voice's priority is too high for `limit` under `rule`. The voice stays in the list.
template <typename Voice>
int FindLowerPriority(const std::vector<int>& next, const std::vector<Voice>& voices, int head, int limit,
                      PriorityRule rule)
{
    int current = next[static_cast<std::size_t>(head)];
    if (current == head)
    {
        return kNoVoice;
    }

    int best = current;
    for (; current != head; current = next[static_cast<std::size_t>(current)])
    {
        if (voices[static_cast<std::size_t>(best)].priority >= voices[static_cast<std::size_t>(current)].priority)
        {
            best = current;
        }
    }

    const int priority = voices[static_cast<std::size_t>(best)].priority;
    const bool too_high = rule == PriorityRule::kLowerOrEqual ? limit < priority : limit <= priority;

    return too_high ? kNoVoice : best;
}

// Takes the voice that FindLowerPriority finds out of its list and returns it, or returns kNoVoice.
template <typename Voice>
int PopLowerPriority(std::vector<int>& next, std::vector<int>& previous, const std::vector<Voice>& voices, int head,
                     int limit, PriorityRule rule)
{
    const int voice = FindLowerPriority(next, voices, head, limit, rule);
    if (voice != kNoVoice)
    {
        Remove(next, previous, voice);
    }

    return voice;
}

// Takes the last disabled voice of a pool for a new note, and puts it at the start of the pool's active list. `take`
// sets the voice up for the note. Returns the voice, or kNoVoice when the pool has no disabled voice.
template <typename Voice, typename Take>
int AllocFromDisabled(std::vector<int>& next, std::vector<int>& previous, std::vector<Voice>& voices, int pool,
                      const Take& take)
{
    const auto voice_count = static_cast<int>(voices.size());
    const int voice = PopBack(next, previous, Head(voice_count, pool, EadListKind::kDisabled));
    if (voice != kNoVoice)
    {
        take(voice);
        PushFront(next, previous, voices, Head(voice_count, pool, EadListKind::kActive), voice);
    }

    return voice;
}

// Takes the last decaying voice of a pool for a new note, and puts it at the end of the pool's releasing list. `take`
// starts the voice's release and hands the voice to the note. Returns the voice, or kNoVoice when the pool has no
// decaying voice.
template <typename Voice, typename Take>
int AllocFromDecaying(std::vector<int>& next, std::vector<int>& previous, std::vector<Voice>& voices, int pool,
                      const Take& take)
{
    const auto voice_count = static_cast<int>(voices.size());
    const int voice = PopBack(next, previous, Head(voice_count, pool, EadListKind::kDecaying));
    if (voice != kNoVoice)
    {
        take(voice);
        PushBack(next, previous, voices, Head(voice_count, pool, EadListKind::kReleasing), voice);
    }

    return voice;
}

} // namespace ultralib::n64::ead_voice_lists
