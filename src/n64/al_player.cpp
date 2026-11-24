// SPDX-License-Identifier: MIT

// libultra's two sequence players: the queue of events, the oscillators and the events of the sequences.
// al_player_midi.cpp contains the channel messages, al_player_voices.cpp the voices and al_player_sequences.cpp the
// readers of the sequences.

#include "ultralib/n64/al_player.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "al_player_constants.h"

namespace ultralib::n64
{

namespace
{

using al_player_constants::kVolumeFull;

constexpr int32_t kFrameMicroseconds = 16000;
constexpr int32_t kQueueEnd = 0x7FFFFFFF;
constexpr int32_t kKillTime = 50000;
constexpr float kInitialTempo = 500000.0F;
constexpr int32_t kOscillatorStep = 16000;
constexpr float kTwoPi = 6.2831854820251465F;
constexpr uint8_t kOscillatorTremoloSine = 1;
constexpr uint8_t kOscillatorTremoloSquare = 2;
constexpr uint8_t kOscillatorTremoloFall = 3;
constexpr uint8_t kOscillatorTremoloRise = 4;
constexpr uint8_t kOscillatorVibratoSine = 128;
constexpr uint8_t kOscillatorVibratoSquare = 129;
constexpr uint8_t kOscillatorVibratoFall = 130;
constexpr uint8_t kOscillatorVibratoRise = 131;

// libultra's sine, in double precision from a single-precision angle.
float Sine(float angle)
{
    static constexpr double kFirst = -0.16666659550427756;
    static constexpr double kSecond = 0.008333066246082155;
    static constexpr double kThird = -0.0001980960290193795;
    static constexpr double kFourth = 2.605780637968037e-06;
    static constexpr double kOneOverPi = 0.3183098861837907;
    static constexpr double kPiHigh = 3.1415926218032837;
    static constexpr double kPiLow = 3.178650954705639e-08;

    const auto polynomial = [](double x)
    {
        const double square = x * x;
        const double fourth_term = kFourth * square;
        const double a = fourth_term + kThird;
        const double b = a * square;
        const double c = b + kSecond;
        const double d = c * square;
        const double sum = kFirst + d;
        const double cube = x * square;
        const double term = cube * sum;

        return term + x;
    };

    const int exponent = static_cast<int>((std::bit_cast<uint32_t>(angle) >> 22) & 0x1FF);
    if (exponent < 255)
    {
        return exponent >= 230 ? static_cast<float>(polynomial(angle)) : angle;
    }

    if (exponent >= 310)
    {
        return 0.0F;
    }

    const auto wide = static_cast<double>(angle);
    const double turns = wide * kOneOverPi;
    const auto half_turns = static_cast<int32_t>(turns >= 0.0 ? turns + 0.5 : turns - 0.5);
    const auto halves = static_cast<double>(half_turns);
    const double high = halves * kPiHigh;
    const double low = halves * kPiLow;
    const double first = wide - high;
    const double reduced = first - low;
    const auto result = static_cast<float>(polynomial(reduced));

    return (half_turns & 1) != 0 ? -result : result;
}

// The example code's depth of a vibrato in cents: a power of a fixed step.
float DepthToCents(uint8_t depth)
{
    float step = 1.0309929847717285F;
    float cents = 1.0F;
    for (int bits = depth; bits != 0; bits >>= 1)
    {
        if ((bits & 1) != 0)
        {
            cents *= step;
        }

        step *= step;
    }

    return cents;
}

int32_t UpdateOscillatorState(uint8_t type, bool& second_half, uint16_t period, uint16_t& count, uint8_t level,
                              uint8_t depth, uint8_t low_level, float cents, float low_ratio, float high_ratio,
                              int32_t start_cents, int32_t cents_range, float& value)
{
    const auto next_part = [&](bool wraps_at_period)
    {
        count++;
        if (wraps_at_period ? count >= period : count > period)
        {
            count = 0;
        }

        return static_cast<float>(count) / static_cast<float>(period);
    };

    switch (type)
    {
    case kOscillatorTremoloSine:
        {
            const float angle = next_part(true) * kTwoPi;
            const float wave = static_cast<float>(depth) * Sine(angle);
            value = static_cast<float>(level) + wave;
            break;
        }

    case kOscillatorTremoloSquare:
        value = static_cast<float>(second_half ? level : low_level);
        second_half = !second_half;

        return static_cast<int32_t>(period) * kOscillatorStep;

    case kOscillatorTremoloFall:
        {
            const float amount = next_part(false) * static_cast<float>(depth);
            value = static_cast<float>(level) - amount;
            break;
        }

    case kOscillatorTremoloRise:
        {
            const float amount = next_part(false) * static_cast<float>(depth);
            value = static_cast<float>(level) + amount;
            break;
        }

    case kOscillatorVibratoSine:
        {
            const float angle = next_part(true) * kTwoPi;
            const float amount = Sine(angle) * cents;
            value = CentsToRatio(static_cast<int32_t>(amount));
            break;
        }

    case kOscillatorVibratoSquare:
        value = second_half ? high_ratio : low_ratio;
        second_half = !second_half;

        return static_cast<int32_t>(period) * kOscillatorStep;

    case kOscillatorVibratoFall:
        {
            const float amount = next_part(false) * static_cast<float>(cents_range);
            const float now = static_cast<float>(start_cents) - amount;
            value = CentsToRatio(static_cast<int32_t>(now));
            break;
        }

    case kOscillatorVibratoRise:
        {
            const float amount = next_part(false) * static_cast<float>(cents_range);
            const float now = amount + static_cast<float>(start_cents);
            value = CentsToRatio(static_cast<int32_t>(now));
            break;
        }

    default:
        break;
    }

    return kOscillatorStep;
}

} // namespace

float CentsToRatio(int32_t cents)
{
    float step = 1.00057779F;
    if (cents < 0)
    {
        step = 0.9994225441F;
        cents = -cents;
    }

    float ratio = 1.0F;
    while (cents != 0)
    {
        if ((cents & 1) != 0)
        {
            ratio *= step;
        }

        step *= step;
        cents >>= 1;
    }

    return ratio;
}

AlWave WaveOfSound(const AlSoundBank& bank, const BankSound& sound)
{
    const BankWave& wave = bank.file->waves[static_cast<std::size_t>(sound.wave)];
    AlWave out;
    out.raw = wave.type == kWaveRaw16;
    out.base = bank.sample_address + wave.base;
    out.length = wave.length;
    if (wave.loop != kNoObject)
    {
        const BankLoop& loop = bank.file->loops[static_cast<std::size_t>(wave.loop)];
        out.has_loop = true;
        out.loop_start = loop.start;
        out.loop_end = loop.end;
        out.loop_count = loop.count;
        constexpr uint32_t kLoopStateOffset = 12;
        out.loop_state = bank.file_address + loop.offset + kLoopStateOffset;
    }

    if (wave.book != kNoObject)
    {
        const BankBook& book = bank.file->books[static_cast<std::size_t>(wave.book)];
        constexpr uint32_t kBookCoefficients = 8;
        out.book = bank.file_address + book.offset + kBookCoefficients;
        out.book_size = 2 * book.book.order * book.book.predictor_count * 8;
    }

    return out;
}

AlPlayer::AlPlayer(const AlPlayerConfig& config, AlSynth& synth)
    : channels_(config.channels),
      max_events_(std::max(config.events, 1)),
      library_form_(config.form),
      oscillators_(config.oscillators)
{
    if (library_form_ == AlPlayerForm::kPlayerScale)
    {
        volume_ = 0x7800;
    }

    next_event_.type = EventType::kPeriodic;
    channels_state_.resize(static_cast<std::size_t>(channels_));
    voices_.resize(static_cast<std::size_t>(config.voices));
    const int first = synth.AddVoices(config.voices);

    // The free list's head is the voice added last. The back of the vector is the head.
    for (int i = 0; i < config.voices; i++)
    {
        voices_[static_cast<std::size_t>(i)].synth_voice = first + i;
        free_voices_.push_back(i);
    }

    samples_left = synth.CurrentSamples();
}

void AlPlayer::Post(const Event& event, int32_t delta)
{
    if (static_cast<int>(queue_.size()) >= max_events_)
    {
        return;
    }

    const bool at_end = delta == kQueueEnd;
    for (std::size_t i = 0; i <= queue_.size(); i++)
    {
        if (i == queue_.size())
        {
            queue_.push_back({event, at_end ? 0 : delta});
            return;
        }

        if (delta < queue_[i].delta)
        {
            queue_[i].delta -= delta;
            queue_.insert(queue_.begin() + static_cast<std::ptrdiff_t>(i), QueueItem{event, delta});
            return;
        }

        delta -= queue_[i].delta;
    }
}

int32_t AlPlayer::NextEvent(Event& event)
{
    if (queue_.empty())
    {
        event.type = EventType::kPeriodic;

        return 0;
    }

    event = queue_.front().event;
    const int32_t delta = queue_.front().delta;
    queue_.erase(queue_.begin());

    return delta;
}

void AlPlayer::FlushType(EventType type)
{
    for (std::size_t i = 0; i < queue_.size();)
    {
        if (queue_[i].event.type == type)
        {
            if (i + 1 < queue_.size())
            {
                queue_[i + 1].delta += queue_[i].delta;
            }

            queue_.erase(queue_.begin() + static_cast<std::ptrdiff_t>(i));
        }
        else
        {
            i++;
        }
    }
}

void AlPlayer::StopOscillators(int voice)
{
    VoiceState& state = voices_[static_cast<std::size_t>(voice)];
    for (std::size_t i = 0; i < queue_.size() && state.oscillators != 0;)
    {
        const Event& event = queue_[i].event;
        if ((event.type != EventType::kTremolo && event.type != EventType::kVibrato) || event.voice != voice)
        {
            i++;
            continue;
        }

        state.oscillators &= event.type == EventType::kTremolo ? uint8_t{0xFE} : uint8_t{0xFD};
        free_oscillators_.push_back(event.oscillator);
        if (i + 1 < queue_.size())
        {
            queue_[i + 1].delta += queue_[i].delta;
        }

        queue_.erase(queue_.begin() + static_cast<std::ptrdiff_t>(i));
    }
}

int32_t AlPlayer::StartOscillator(int& state, float& value, uint8_t type, uint8_t rate, uint8_t depth, uint8_t delay)
{
    if (free_oscillators_.empty())
    {
        free_oscillators_.push_back(static_cast<int>(oscillator_states_.size()));
        oscillator_states_.emplace_back();
    }

    state = free_oscillators_.back();
    free_oscillators_.pop_back();
    Oscillator& oscillator = oscillator_states_[static_cast<std::size_t>(state)];
    oscillator = {};
    oscillator.type = type;
    const auto long_period = static_cast<uint16_t>(256 - rate);
    const auto sine_period = static_cast<uint16_t>(259 - rate);
    switch (type)
    {
    case kOscillatorTremoloSine:
        oscillator.period = sine_period;
        oscillator.depth = static_cast<uint8_t>(depth >> 1);
        oscillator.level = static_cast<uint8_t>(kVolumeFull - oscillator.depth);
        value = static_cast<float>(oscillator.level);
        break;

    case kOscillatorTremoloSquare:
        oscillator.period = long_period;
        oscillator.count = long_period;
        oscillator.low_level = static_cast<uint8_t>(kVolumeFull - depth);
        oscillator.level = kVolumeFull;
        value = static_cast<float>(kVolumeFull);
        break;

    case kOscillatorTremoloFall:
        oscillator.period = long_period;
        oscillator.level = kVolumeFull;
        oscillator.depth = depth;
        value = static_cast<float>(kVolumeFull);
        break;

    case kOscillatorTremoloRise:
        oscillator.period = long_period;
        oscillator.depth = depth;
        oscillator.level = static_cast<uint8_t>(kVolumeFull - depth);
        value = static_cast<float>(oscillator.level);
        break;

    case kOscillatorVibratoSine:
        oscillator.period = sine_period;
        oscillator.cents = DepthToCents(depth);
        value = 1.0F;
        break;

    case kOscillatorVibratoSquare:
        {
            oscillator.period = long_period;
            oscillator.count = long_period;
            const auto cents = static_cast<int32_t>(DepthToCents(depth));
            oscillator.low_ratio = CentsToRatio(-cents);
            oscillator.high_ratio = CentsToRatio(cents);
            value = oscillator.high_ratio;
            break;
        }

    case kOscillatorVibratoFall:
        {
            oscillator.period = long_period;
            oscillator.count = long_period;
            const auto cents = static_cast<int32_t>(DepthToCents(depth));
            oscillator.start_cents = cents;
            oscillator.cents_range = cents * 2;
            value = CentsToRatio(cents);
            break;
        }

    case kOscillatorVibratoRise:
        {
            oscillator.period = long_period;
            oscillator.count = long_period;
            const auto cents = static_cast<int32_t>(DepthToCents(depth));
            oscillator.start_cents = -cents;
            oscillator.cents_range = cents * 2;
            value = CentsToRatio(-cents);
            break;
        }

    default:
        break;
    }

    return static_cast<int32_t>(delay) << 14;
}

void AlPlayer::RemoveEnvelopeEvents(int voice)
{
    for (std::size_t i = 0; i < queue_.size();)
    {
        if (queue_[i].event.type == EventType::kEnvelope && queue_[i].event.voice == voice)
        {
            if (i + 1 < queue_.size())
            {
                queue_[i + 1].delta += queue_[i].delta;
            }

            queue_.erase(queue_.begin() + static_cast<std::ptrdiff_t>(i));
        }
        else
        {
            i++;
        }
    }
}

bool AlPlayer::NeedsNoteKill(int voice, int32_t kill_time)
{
    int32_t item_time = 0;
    for (std::size_t i = 0; i < queue_.size(); i++)
    {
        item_time += queue_[i].delta;
        if (queue_[i].event.type == EventType::kNoteEnd && queue_[i].event.voice == voice)
        {
            if (item_time > kill_time)
            {
                if (i + 1 < queue_.size())
                {
                    queue_[i + 1].delta += queue_[i].delta;
                }

                queue_.erase(queue_.begin() + static_cast<std::ptrdiff_t>(i));

                return true;
            }

            return false;
        }
    }

    return true;
}

void AlPlayer::SetBank(const AlSoundBank& bank)
{
    file_ = bank.file;
    bank_ = bank.bank;
    file_address_ = bank.file_address;
    sample_address_ = bank.sample_address;
    Event event;
    event.type = EventType::kSetBank;
    Post(event, 0);
}

void AlPlayer::SetSequence(AlSequenceForm form, std::span<const uint8_t> bytes)
{
    pending_bytes_.assign(bytes.begin(), bytes.end());
    form_ = form;
    Event event;
    event.type = EventType::kSetSequence;
    Post(event, 0);
}

void AlPlayer::SetVolume(int16_t volume)
{
    Event event;
    event.type = EventType::kPlayerVolume;
    event.player_volume = volume;
    Post(event, 0);
}

void AlPlayer::SetFxOffset(int8_t offset)
{
    fx_offset_ = offset;
}

void AlPlayer::Play()
{
    Event event;
    event.type = EventType::kPlay;
    Post(event, 0);
}

void AlPlayer::Stop()
{
    Event event;
    event.type = EventType::kStopping;
    Post(event, 0);
}

void AlPlayer::SetLoop(int32_t start_ticks, int32_t end_ticks, int32_t count)
{
    Event event;
    event.type = EventType::kSetLoop;
    event.loop_start = start_ticks;
    event.loop_end = end_ticks;
    event.loop_count = count;
    Post(event, 0);
}

void AlPlayer::SendMidi(uint8_t status, uint8_t byte1, uint8_t byte2)
{
    Event event;
    event.type = EventType::kMidi;
    event.status = status;
    event.byte1 = byte1;
    event.byte2 = byte2;
    Post(event, 0);
}

AlPlayerState AlPlayer::State() const
{
    return state_;
}

int32_t AlPlayer::Time() const
{
    return time_;
}

int64_t AlPlayer::TickPosition() const
{
    return form_ == AlSequenceForm::kCompressed ? last_ticks_ : standard_ticks_;
}

int32_t AlPlayer::Handle(AlSynth& synth)
{
    int32_t next_delta = 0;
    do
    {
        switch (next_event_.type)
        {
        case EventType::kSequence:
            HandleSequenceEvent(synth);
            break;

        case EventType::kPeriodic:
            {
                Event event;
                event.type = EventType::kPeriodic;
                Post(event, kFrameMicroseconds);
                break;
            }

        case EventType::kNoteEnd:
            EndVoice(synth, next_event_.voice);
            break;

        case EventType::kTremolo:
        case EventType::kVibrato:
            StepOscillator(synth);
            break;

        case EventType::kEnvelope:
            NextEnvelopeSegment(synth);
            break;

        case EventType::kMidi:
        case EventType::kNoteOff:
            HandleMidi(synth, next_event_);
            break;

        case EventType::kPlayerVolume:
            volume_ = next_event_.player_volume;
            for (const int index : active_voices_)
            {
                ChangeVoiceLevel(synth, index);
            }
            break;

        case EventType::kPlay:
            if (state_ != AlPlayerState::kPlaying)
            {
                state_ = AlPlayerState::kPlaying;
                PostNextSequenceEvent();
            }
            break;

        case EventType::kStop:
            FinishStop(synth);
            break;

        case EventType::kStopping:
            StartStopping(synth);
            break;

        case EventType::kSetSequence:
            LoadSequence();
            break;

        case EventType::kSetBank:
            InitFromBank();
            break;

        case EventType::kSetLoop:
            loop_start_ = MakeMarker(next_event_.loop_start);
            loop_end_ = MakeMarker(next_event_.loop_end);
            loop_count_ = next_event_.loop_count;
            break;
        }

        next_delta = NextEvent(next_event_);
    } while (next_delta == 0);

    time_ += next_delta;

    return next_delta;
}

// The envelope segment that the event starts: the voice's target level and the time to reach it.
void AlPlayer::NextEnvelopeSegment(AlSynth& synth)
{
    VoiceState& voice = voices_[static_cast<std::size_t>(next_event_.voice)];
    if (voice.envelope_phase == EnvelopePhase::kAttack)
    {
        voice.envelope_phase = EnvelopePhase::kDecay;
    }

    const int32_t delta = next_event_.delta;
    voice.envelope_gain = next_event_.volume;
    voice.envelope_end = time_ + delta;
    synth.SetVolume(voice.synth_voice, VoiceVolume(voice), delta);
}

// Stops a voice and frees it.
void AlPlayer::EndVoice(AlSynth& synth, int voice)
{
    const int synth_voice = voices_[static_cast<std::size_t>(voice)].synth_voice;
    synth.StopVoice(synth_voice);
    synth.FreeVoice(synth_voice);
    StopOscillators(voice);
    UnmapVoice(voice);
}

// The next step of a voice's tremolo or vibrato.
void AlPlayer::StepOscillator(AlSynth& synth)
{
    VoiceState& voice = voices_[static_cast<std::size_t>(next_event_.voice)];
    Oscillator& oscillator = oscillator_states_[static_cast<std::size_t>(next_event_.oscillator)];
    float value = 0.0F;
    const int32_t delay = UpdateOscillatorState(
        oscillator.type, oscillator.second_half, oscillator.period, oscillator.count, oscillator.level,
        oscillator.depth, oscillator.low_level, oscillator.cents, oscillator.low_ratio, oscillator.high_ratio,
        oscillator.start_cents, oscillator.cents_range, value);
    if (next_event_.type == EventType::kTremolo)
    {
        voice.tremolo = static_cast<uint8_t>(static_cast<int32_t>(value));
        synth.SetVolume(voice.synth_voice, VoiceVolume(voice), VoiceDelta(voice));
    }
    else
    {
        voice.vibrato = value;
        const float pitch = voice.pitch * voice.vibrato;
        synth.SetPitch(voice.synth_voice, channels_state_[voice.channel].pitch_bend * pitch);
    }

    Post(next_event_, delay);
}

// The end of a stop: every voice ends, and the player stops.
void AlPlayer::FinishStop(AlSynth& synth)
{
    if (state_ != AlPlayerState::kStopping)
    {
        return;
    }

    while (!active_voices_.empty())
    {
        EndVoice(synth, active_voices_.front());
    }

    // Only ALSeqPlayer starts its time again.
    if (form_ == AlSequenceForm::kStandard)
    {
        time_ = 0;
    }

    state_ = AlPlayerState::kStopped;
}

// The start of a stop: the sequence's events leave the queue, and the voices release over the kill time.
void AlPlayer::StartStopping(AlSynth& synth)
{
    if (state_ != AlPlayerState::kPlaying)
    {
        return;
    }

    FlushType(EventType::kSequence);
    if (form_ == AlSequenceForm::kCompressed)
    {
        FlushType(EventType::kNoteOff);
    }

    FlushType(EventType::kMidi);
    for (const int index : std::vector<int>(active_voices_))
    {
        if (NeedsNoteKill(index, kKillTime))
        {
            ReleaseVoice(synth, index, kKillTime);
        }
    }

    state_ = AlPlayerState::kStopping;
    Event event;
    event.type = EventType::kStop;
    Post(event, kQueueEnd);
}

// Switches to the sequence that SetSequence queued, and starts its reader at the first tempo.
void AlPlayer::LoadSequence()
{
    bytes_ = pending_bytes_;
    has_sequence_ = true;
    if (form_ == AlSequenceForm::kCompressed)
    {
        NewCompressed();
    }
    else
    {
        NewStandard();
    }

    SetTempo(kInitialTempo);
    if (file_ != nullptr)
    {
        InitFromBank();
    }
}

void AlPlayer::PostNextSequenceEvent()
{
    if (state_ != AlPlayerState::kPlaying || !has_sequence_)
    {
        return;
    }

    int32_t ticks = 0;
    if (form_ == AlSequenceForm::kCompressed)
    {
        if (!CompressedNextDelta(ticks))
        {
            return;
        }
    }
    else
    {
        if (!StandardNextDelta(ticks))
        {
            return;
        }

        if (loop_count_ != 0 && standard_ticks_ + ticks >= loop_end_.current_ticks)
        {
            place_ = loop_start_.place;
            last_status_ = loop_start_.last_status;
            standard_ticks_ = loop_start_.last_ticks;
            if (loop_count_ != -1)
            {
                loop_count_--;
            }
        }
    }

    Event event;
    event.type = EventType::kSequence;
    Post(event, ticks * microseconds_per_tick_);
}

void AlPlayer::HandleSequenceEvent(AlSynth& synth)
{
    if (!has_sequence_)
    {
        return;
    }

    const SequenceEvent next = form_ == AlSequenceForm::kCompressed ? CompressedNextEvent() : StandardNextEvent();
    switch (next.type)
    {
    case SequenceEventType::kMidi:
        {
            Event event;
            event.type = EventType::kMidi;
            event.status = next.status;
            event.byte1 = next.byte1;
            event.byte2 = next.byte2;
            event.duration = next.duration;
            event.offset = next.offset;
            HandleMidi(synth, event);
            PostNextSequenceEvent();
            break;
        }

    case SequenceEventType::kTempo:
        HandleTempo(next);
        PostNextSequenceEvent();
        break;

    case SequenceEventType::kEnd:
        {
            state_ = AlPlayerState::kStopping;
            Event event;
            event.type = EventType::kStop;
            Post(event, kQueueEnd);
            break;
        }

    case SequenceEventType::kTrackEnd:
    case SequenceEventType::kLoopStart:
    case SequenceEventType::kLoopEnd:
        PostNextSequenceEvent();
        break;
    }
}

void BuildAlFrame(AlSynth& synth, AlPlayer& player, AudioList& list, AudioMemory& memory, uint32_t output, int samples)
{
    // The player runs for every event that comes before the frame's end. Each call sets the driver's time for updates
    // to the event's time, on a multiple of 16 samples.
    int32_t next = player.samples_left;
    while (next - synth.CurrentSamples() < samples)
    {
        synth.SetParamSamples(next & ~0xF);
        player.samples_left += synth.TimeToSamplesNoRound(player.Handle(synth));
        next = player.samples_left;
    }

    synth.SetParamSamples(next & ~0xF);
    synth.BuildCommands(list, memory, output, samples);
}

} // namespace ultralib::n64
