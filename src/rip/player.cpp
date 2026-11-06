// SPDX-License-Identifier: MIT

#include "rip/player.h"

#include <algorithm>

#include "usf/track.h"

namespace usf::rip
{

namespace
{

uint64_t SignExtend32(uint32_t value)
{
    return static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(value)));
}

} // namespace

TrackPlayer::TrackPlayer(std::span<const uint8_t> rom, std::span<const uint8_t> state, const PlayerOptions& options)
    : system_(TrackOptions(state, options.enable_compare, options.enable_fifo_full)),
      loop_watches_(options.loop_watches)
{
    system_.SetCoverage(options.coverage);
    system_.SetAudioCallback([this](const n64::AudioBuffer& buffer) { Queue(buffer.samples, buffer.sample_rate); });
    StartTrack(system_, rom, state);

    for (const LoopWatch& watch : loop_watches_)
    {
        system_.AddBreakpoint(SignExtend32(watch.address));
    }

    clock_.Start(system_.Now());
}

void TrackPlayer::Render(std::span<int16_t> out)
{
    const uint64_t frames = out.size() / 2;
    Fill(frames);
    for (uint64_t i = 0; i < frames; i++)
    {
        const Frame& frame = queue_.front();
        out[2 * i] = frame.left;
        out[2 * i + 1] = frame.right;
        rate_ = frame.rate;
        queue_.pop_front();
    }

    rendered_ += frames;
}

uint32_t TrackPlayer::SampleRate()
{
    if (rate_ == 0)
    {
        Fill(1);
        rate_ = queue_.front().rate;
    }

    return rate_;
}

uint64_t TrackPlayer::Frames() const
{
    return rendered_;
}

std::vector<LoopEvent> TrackPlayer::TakeLoops(uint64_t frame)
{
    std::vector<LoopEvent> taken;
    const auto first_later =
        std::find_if(loops_.begin(), loops_.end(), [frame](const LoopEvent& loop) { return loop.frame >= frame; });
    taken.assign(loops_.begin(), first_later);
    loops_.erase(loops_.begin(), first_later);

    return taken;
}

n64::System& TrackPlayer::System()
{
    return system_;
}

void TrackPlayer::Fill(uint64_t frames)
{
    while (queue_.size() < frames)
    {
        RunSlice();
    }
}

void TrackPlayer::RunSlice()
{
    system_.Run(OutputClock::SliceTicks(system_.TicksPerSecond()));
    if (system_.HitBreakpoint())
    {
        // The game is about to take a loop. Its output so far ends at the queued frames.
        const n64::Cpu& cpu = system_.GetCpu();
        const uint32_t pc = static_cast<uint32_t>(cpu.Pc());
        for (const LoopWatch& watch : loop_watches_)
        {
            if (watch.address != pc)
            {
                continue;
            }
            if (watch.count_register > 0 && (cpu.Gpr(watch.count_register) & 0xFF) != 0xFF)
            {
                continue;
            }
            if (watch.match_register > 0 && static_cast<uint32_t>(cpu.Gpr(watch.match_register)) != watch.match_value)
            {
                continue;
            }

            LoopEvent loop;
            loop.frame = produced_;
            loop.track = watch.track_register > 0 ? static_cast<int>(cpu.Gpr(watch.track_register)) : 0;
            loop.target = watch.target_register > 0 ? static_cast<uint32_t>(cpu.Gpr(watch.target_register)) : 0;
            loops_.push_back(loop);
        }
    }

    // Output that has stopped is filled with silence up to the present, at the latest rate. Before the game's first
    // buffer, that's the rate that the state's DACRATE gives.
    const uint32_t rate = !queue_.empty() ? queue_.back().rate : (rate_ != 0 ? rate_ : system_.AudioSampleRate());
    const uint64_t silence = clock_.TakeSilence(system_.Now(), rate, system_.TicksPerSecond());
    if (silence > 0)
    {
        queue_.insert(queue_.end(), silence, Frame{0, 0, rate});
        produced_ += silence;
    }
}

void TrackPlayer::Queue(std::span<const int16_t> samples, uint32_t rate)
{
    const uint64_t frames = samples.size() / 2;
    for (uint64_t i = 0; i < frames; i++)
    {
        queue_.push_back({samples[2 * i], samples[2 * i + 1], rate});
    }

    produced_ += frames;
    clock_.OnBuffer(frames, rate, system_.Now(), system_.TicksPerSecond());
}

void SampleHashes::Add(std::span<const int16_t> samples)
{
    size_t pos = 0;
    while (pos < samples.size())
    {
        const uint64_t room = 2 * (kBlockFrames - frames_ % kBlockFrames);
        const size_t take = static_cast<size_t>(std::min<uint64_t>(room, samples.size() - pos));
        std::vector<uint8_t> bytes(2 * take);
        for (size_t i = 0; i < take; i++)
        {
            const auto sample = static_cast<uint16_t>(samples[pos + i]);
            bytes[2 * i] = static_cast<uint8_t>(sample & 0xFF);
            bytes[2 * i + 1] = static_cast<uint8_t>(sample >> 8);
        }

        block_.Update(bytes);
        frames_ += take / 2;
        pos += take;

        if (frames_ % kBlockFrames == 0)
        {
            blocks_.push_back(block_.Finish());
            block_ = xsflib::Sha256();
        }
    }
}

const std::vector<xsflib::Sha256::Digest>& SampleHashes::Blocks() const
{
    return blocks_;
}

} // namespace usf::rip
