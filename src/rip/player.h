// SPDX-License-Identifier: MIT

// The ripper's player: the core running a track as a legacy player runs it, with its output in exact runs of stereo
// frames. The length analysis, the coverage runs and the checks of trimmed rips all render through it.

#pragma once

#include <cstdint>
#include <deque>
#include <span>
#include <vector>

#include "xsflib/sha256.h"

#include "n64/system.h"
#include "rip/profile.h"
#include "usf/track.h"

namespace usf::rip
{

// A loop that a loop watch saw: the output frame at which the game took it, and the values of the watch's registers.
struct LoopEvent
{
    uint64_t frame = 0;
    int track = 0;
    uint32_t target = 0;
};

struct PlayerOptions
{
    // The track's two flags. The core then runs as a legacy player runs the track (usf::TrackOptions).
    bool enable_compare = true;
    bool enable_fifo_full = true;

    // Record coverage from the start.
    bool coverage = false;

    std::vector<LoopWatch> loop_watches;
};

class TrackPlayer
{
public:
    // `rom` is in console byte order. `state` is a state image.
    TrackPlayer(std::span<const uint8_t> rom, std::span<const uint8_t> state, const PlayerOptions& options);

    TrackPlayer(const TrackPlayer&) = delete;
    TrackPlayer& operator=(const TrackPlayer&) = delete;

    // Renders out.size() / 2 stereo frames into `out`.
    void Render(std::span<int16_t> out);

    // The rate of the latest buffer that Render took frames from, or of the first buffer to come.
    uint32_t SampleRate();

    // The frames rendered so far.
    uint64_t Frames() const;

    // Takes the loops seen before output frame `frame`.
    std::vector<LoopEvent> TakeLoops(uint64_t frame);

    n64::System& System();

private:
    struct Frame
    {
        int16_t left;
        int16_t right;
        uint32_t rate;
    };

    // Runs the core until it has queued at least `frames` frames.
    void Fill(uint64_t frames);

    void RunSlice();
    void Queue(std::span<const int16_t> samples, uint32_t rate);

    n64::System system_;
    std::vector<LoopWatch> loop_watches_;

    // Frames waiting to be rendered, each with its rate.
    std::deque<Frame> queue_;

    uint64_t produced_ = 0; // Frames queued so far, rendered or not.
    uint64_t rendered_ = 0;
    uint32_t rate_ = 0;

    OutputClock clock_;

    std::vector<LoopEvent> loops_;
};

// SHA-256 hashes of a render's samples, one for each block of kBlockFrames stereo frames. Two renders are the same over
// their first N frames when their hashes of the blocks that start before N are the same.
class SampleHashes
{
public:
    static constexpr uint64_t kBlockFrames = 32768;

    void Add(std::span<const int16_t> samples);

    // The hashes of the complete blocks.
    const std::vector<xsflib::Sha256::Digest>& Blocks() const;

private:
    std::vector<xsflib::Sha256::Digest> blocks_;
    xsflib::Sha256 block_;
    uint64_t frames_ = 0;
};

} // namespace usf::rip
