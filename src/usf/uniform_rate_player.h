// SPDX-License-Identifier: MIT

// The player of the front ends: usf::Player with one rate for each track. xsflib's Playback counts a track's length in
// frames, a WAV file holds one rate, and XMPlay takes one rate for each track. Start therefore settles the track's
// rate. It's the rate of the track's first frames. A game may change its rate later. The frames at any other rate are
// then converted to the track's rate by linear interpolation. Every front end plays a file alike, and the track's times
// stay right in seconds.
//
// xsflib's Playback uses this class as its player. Playback seeks by restarting and rendering.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "usf/player.h"

namespace usf
{

class UniformRatePlayer : public Player
{
public:
    // The rate depends on the set (xsflib/player/playback.h).
    static constexpr int kSampleRate = 0;

    // SampleRate's estimate before the first Start, when the set hasn't given its rate yet.
    static constexpr int kEstimatedRate = 32000;

    // Starts the track and settles its rate. Returns false after an error.
    bool Start();

    // Renders `frames` stereo frames at the track's rate. The count is less than `frames` only after an error.
    std::size_t Render(int16_t* out, std::size_t frames);

    // The track's rate: an estimate before the first Start.
    int SampleRate() const;

private:
    using Frame = std::array<int16_t, 2>;

    // Renders frames at `rate` until the rate changes. Returns the frames written.
    std::size_t RenderConverted(int16_t* out, std::size_t frames, uint32_t rate);

    uint32_t rate_ = kEstimatedRate;

    // The latest frame that left the player unconverted, where a conversion starts from.
    Frame last_{};

    // The conversion: the next output frame lies phase_ / rate_ of the way from from_ to to_.
    bool converting_ = false;
    Frame from_{};
    Frame to_{};
    uint64_t phase_ = 0;
};

} // namespace usf
