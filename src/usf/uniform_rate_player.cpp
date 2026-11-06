// SPDX-License-Identifier: MIT

#include "usf/uniform_rate_player.h"

namespace usf
{

bool UniformRatePlayer::Start()
{
    if (!Player::Start())
    {
        return false;
    }

    rate_ = Player::SampleRate();
    converting_ = false;
    last_ = {};

    return rate_ != 0;
}

std::size_t UniformRatePlayer::Render(int16_t* out, std::size_t frames)
{
    std::size_t done = 0;
    while (done < frames)
    {
        const uint32_t rate = Player::SampleRate();
        if (rate == 0)
        {
            break;
        }

        int16_t* const to = out + 2 * done;
        std::size_t count = 0;
        if (rate != rate_)
        {
            count = RenderConverted(to, frames - done, rate);
        }
        else
        {
            count = Player::Render(to, frames - done);
            converting_ = false;
            if (count > 0)
            {
                last_ = {to[2 * count - 2], to[2 * count - 1]};
            }
        }

        if (count == 0)
        {
            break;
        }

        done += count;
    }

    return done;
}

int UniformRatePlayer::SampleRate() const
{
    return static_cast<int>(rate_);
}

std::size_t UniformRatePlayer::RenderConverted(int16_t* out, std::size_t frames, uint32_t rate)
{
    if (!converting_)
    {
        converting_ = true;
        from_ = last_;
        to_ = last_;
        phase_ = rate_;
    }

    for (std::size_t i = 0; i < frames; i++)
    {
        // Each output frame moves on by rate / rate_ of a source frame. Bring in the source frames that it lies
        // between, unless the rate changes first.
        while (phase_ >= rate_)
        {
            if (Player::SampleRate() != rate)
            {
                return i;
            }

            from_ = to_;
            if (Player::Render(to_.data(), 1) == 0)
            {
                return i;
            }

            phase_ -= rate_;
        }

        for (std::size_t channel = 0; channel < 2; channel++)
        {
            const int64_t difference = int64_t{to_[channel]} - from_[channel];
            const int64_t step = difference * static_cast<int64_t>(phase_) / static_cast<int64_t>(rate_);
            out[2 * i + channel] = static_cast<int16_t>(from_[channel] + step);
        }

        phase_ += rate;
    }

    return frames;
}

} // namespace usf
