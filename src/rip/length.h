// SPDX-License-Identifier: MIT

// The length analysis: xsflib's MeasureLength on the output of the game. A frame is 1 ms of output. A frame sounds when
// one of its samples is louder than xsflib's silence level. The loops come from the profile's loop watches. The
// analysis follows no voices: it knows neither when the game's sequence player is busy nor when it has stopped. A track
// therefore ends when it has looped twice, when it has been silent for the silence's length, or at the limit.

#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

#include "xsflib/rip/length.h"

#include "rip/player.h"

namespace usf::rip
{

// The longest a track plays: the limit of the length analysis, and the run of a track without a length.
inline constexpr double kMaxTrackSeconds = 600.0;

// The rules, as in the other rippers that run a game's code: frames of 1 ms, a limit of 600 s, a silence of 180 s that
// ends a track, two loops, a fade of 10 s, and 0.5 s of silence after the last sound of a track that ends.
xsflib::rip::LengthRules GameLengthRules();

// Renders a player's output 1 ms at a time. Each millisecond holds the frames that the current rate gives it. The
// remainders carry over, so that the frames of 1,000 ms add up to the rate.
class MsRenderer
{
public:
    explicit MsRenderer(TrackPlayer& player);

    // The samples of the next millisecond. They stay valid until the next call.
    std::span<const int16_t> Next();

    // The milliseconds rendered so far.
    uint64_t Ms() const;

    TrackPlayer& Player();

private:
    TrackPlayer& player_;
    std::vector<int16_t> samples_;
    uint64_t remainder_ = 0;
    uint64_t ms_ = 0;
};

// Measures the track that `renderer` plays, from where it stands. Every millisecond rendered also goes to `on_samples`.
// Returns nothing if the analysis fails.
std::optional<xsflib::rip::Timing> MeasureTrack(MsRenderer& renderer,
                                                const std::function<void(std::span<const int16_t>)>& on_samples);

} // namespace usf::rip
