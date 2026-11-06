// SPDX-License-Identifier: MIT

// Starting the emulator on a loaded track: the core's options for the track, then its ROM image and its state.

#pragma once

#include <cstdint>
#include <span>

#include "n64/system.h"
#include "usf/usf_file.h"

namespace usf
{

// The core's options for a track: the legacy players' timing, the RDRAM size that `state` gives, the timer interrupt
// only with `_enablecompare`, the AI's delay only with `_enableFIFOfull`, the delays of the PI, the SI and the DP only
// with both, an FPU that raises no exception, and the legacy players' timing of events. `state` must be a state image
// that LoadUsf accepted.
n64::Options TrackOptions(std::span<const uint8_t> state, bool enable_compare, bool enable_fifo_full);
n64::Options TrackOptions(const LoadedUsf& track);

// Loads `rom` (console byte order) and then `state` into `system`, which TrackOptions made.
void StartTrack(n64::System& system, std::span<const uint8_t> rom, std::span<const uint8_t> state);

// Keeps a track's output in time with the emulated time. The output is the concatenation of the game's buffers. A game
// that stops sending them for longer than kStallSeconds gets silence for the time that it stopped, so that time goes
// on in the output too. A player runs the core in slices of SliceTicks, and asks for the silence after each one.
class OutputClock
{
public:
    static constexpr double kSliceSeconds = 0.004;
    static constexpr double kStallSeconds = 0.25;

    static uint64_t SliceTicks(uint64_t ticks_per_second);

    // The track starts at `now`, in ticks of the core's clock.
    void Start(uint64_t now);

    // The game started a buffer of `frames` frames at `rate` at `now`.
    void OnBuffer(uint64_t frames, uint32_t rate, uint64_t now, uint64_t ticks_per_second);

    // The frames of silence at `rate` that the output needs after a slice that ended at `now`. It's 0 unless the output
    // has stalled.
    uint64_t TakeSilence(uint64_t now, uint32_t rate, uint64_t ticks_per_second);

private:
    uint64_t covered_until_ = 0; // The emulated time up to which the output reaches, in ticks.
};

} // namespace usf
