// SPDX-License-Identifier: MIT

// The length analysis: xsflib's MeasureLength on the output of the game. A frame is 1 ms of output. A frame sounds when
// one of its samples is louder than xsflib's silence level. The loops come from the profile's loop watches. The
// analysis follows no voices. Nothing in it shows when the game's sequence player is busy or when it has stopped. A
// track therefore ends when it has looped twice, when it has been silent for the silence's length, or at the limit.

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

// The default limit of the length analysis, the length of a track at either limit, and the run of a track without a
// length. The long limit of music is xsflib's kLongLimitMs.
inline constexpr double kMaxTrackSeconds = 600.0;

// The rules: frames of 1 ms, a limit of 600 s, or xsflib's 3,600 s with the long limit (xsflib::rip::WithLongLimit), a
// silence of 180 s that ends a track, and xsflib's defaults for the rest: two loops and a fade of 7 s. A track that
// ends ends at its last sound. A track at either limit gets 600 s and the fade.
xsflib::rip::LengthRules GameLengthRules(xsflib::rip::Limit limit = xsflib::rip::Limit::kDefault);

// Renders a player's output 1 ms at a time. Each millisecond contains the frames that the current rate gives it. The
// remainders carry over. The frames of 1,000 ms then add up to the rate.
class MsRenderer
{
public:
    explicit MsRenderer(TrackPlayer& player);

    // The samples of the next millisecond. They stay valid until the next call.
    std::span<const int16_t> Next();

    // The milliseconds rendered so far.
    uint64_t Ms() const;

    // The time from the start of the run to the first sample of the latest millisecond that isn't 0, in either channel,
    // in milliseconds. Nothing when every sample of that millisecond is 0. The frames before a change of rate count at
    // the rate that they played at, to within the millisecond of the change.
    std::optional<double> FirstSampleMs() const;

    TrackPlayer& Player();

private:
    TrackPlayer& player_;
    std::vector<int16_t> samples_;
    uint64_t remainder_ = 0;
    uint64_t ms_ = 0;

    // The frames before the latest millisecond, and its rate.
    uint64_t start_frame_ = 0;
    uint32_t rate_ = 0;

    // The run of frames at one rate that the latest millisecond ends: its first frame, that frame's time in
    // milliseconds (0 while the rate hasn't changed), and its rate.
    uint64_t run_start_frame_ = 0;
    double run_start_ms_ = 0.0;
    uint32_t run_rate_ = 0;
};

// Measures the track that `renderer` plays, from where it stands, up to `limit`. The analysis has no second measure: a
// track that reaches 600 s under the long limit plays on to 3,600 s in the same run, as a second run would play it.
// Every millisecond rendered also goes to `on_samples`. Returns nothing if the analysis fails.
std::optional<xsflib::rip::Timing> MeasureTrack(MsRenderer& renderer,
                                                const std::function<void(std::span<const int16_t>)>& on_samples,
                                                xsflib::rip::Limit limit = xsflib::rip::Limit::kDefault);

} // namespace usf::rip
