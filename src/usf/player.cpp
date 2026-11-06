// SPDX-License-Identifier: MIT

#include "usf/player.h"

#include <algorithm>
#include <span>
#include <utility>

namespace usf
{

namespace
{

constexpr std::size_t kSeekBlockFrames = 4096;

} // namespace

std::optional<std::string> Player::LoadFile(const std::string& path, const xsflib::psf::FileReader& reader,
                                            LoadedUsf& set)
{
    return LoadUsf(path, reader, set);
}

Player::Player() = default;

Player::~Player() = default;

std::size_t Player::Render(int16_t* out, std::size_t frames)
{
    const bool running = state_ == State::kPlaying || state_ == State::kFinished;
    if (!running || !Buffer(frames))
    {
        return Base::Render(out, frames);
    }

    // All the frames of one call have one rate.
    const uint64_t position = Position();
    DropPastRates(position);
    if (rates_.size() > 1 && rates_[1].frame < position + frames)
    {
        frames = static_cast<std::size_t>(rates_[1].frame - position);
    }

    return Base::Render(out, frames);
}

uint32_t Player::SampleRate()
{
    const bool running = state_ == State::kPlaying || state_ == State::kFinished;
    if (!running || !Buffer(1))
    {
        return 0;
    }

    DropPastRates(Position());

    return rates_.front().rate;
}

uint64_t Player::Position() const
{
    return (dropped_ + consumed_) / 2;
}

bool Player::Seek(uint64_t frame)
{
    if (frame < Position() && !Start())
    {
        return false;
    }

    scratch_.resize(kSeekBlockFrames * 2);
    while (Position() < frame)
    {
        const uint64_t left = frame - Position();
        if (Render(scratch_.data(), static_cast<std::size_t>(std::min<uint64_t>(left, kSeekBlockFrames))) == 0)
        {
            return false;
        }
    }

    return true;
}

void Player::Unload()
{
    system_.reset();
    samples_.clear();
    rates_.clear();
}

bool Player::Playable() const
{
    return !set_.state.empty();
}

bool Player::StartSource()
{
    system_ = std::make_unique<n64::System>(TrackOptions(set_));
    samples_.clear();
    rates_.clear();

    system_->SetAudioCallback(
        [this](const n64::AudioBuffer& buffer)
        {
            clock_.OnBuffer(buffer.samples.size() / 2, buffer.sample_rate, system_->Now(), system_->TicksPerSecond());
            AddSamples(buffer.samples, buffer.sample_rate);
            silence_rate_ = buffer.sample_rate;
        });

    StartTrack(*system_, set_.rom, set_.state);
    clock_.Start(system_->Now());
    silence_rate_ = system_->AudioSampleRate();

    return true;
}

bool Player::RunStep()
{
    system_->Run(OutputClock::SliceTicks(system_->TicksPerSecond()));

    // Output that has stopped is filled with silence, so that time goes on in the output too.
    const uint64_t silence = clock_.TakeSilence(system_->Now(), silence_rate_, system_->TicksPerSecond());
    if (silence > 0)
    {
        const std::vector<int16_t> zeros(static_cast<std::size_t>(silence) * 2, 0);
        AddSamples(zeros, silence_rate_);
    }

    return true;
}

bool Player::Finished() const
{
    return false;
}

std::vector<int16_t>& Player::Output()
{
    return samples_;
}

bool Player::Buffer(std::size_t frames)
{
    while ((samples_.size() - consumed_) / 2 < frames)
    {
        if (!Step())
        {
            return false;
        }
    }

    return true;
}

uint64_t Player::ProducedFrames() const
{
    return (dropped_ + samples_.size()) / 2;
}

void Player::AddSamples(std::span<const int16_t> samples, uint32_t rate)
{
    if (rates_.empty() || rates_.back().rate != rate)
    {
        rates_.push_back({ProducedFrames(), rate});
    }

    samples_.insert(samples_.end(), samples.begin(), samples.end());
}

void Player::DropPastRates(uint64_t frame)
{
    while (rates_.size() > 1 && rates_[1].frame <= frame)
    {
        rates_.erase(rates_.begin());
    }
}

} // namespace usf
