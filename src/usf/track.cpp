// SPDX-License-Identifier: MIT

#include "usf/track.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "xsflib/bytes.h"

#include "n64/save_state.h"

namespace usf
{

n64::Options TrackOptions(std::span<const uint8_t> state, bool enable_compare, bool enable_fifo_full)
{
    n64::Options options;
    options.timing = n64::Timing::kLegacy;
    options.rdram_size = state.size() >= n64::kStateRdramSizeOffset + 4
                             ? xsflib::LoadLe32(state.data() + n64::kStateRdramSizeOffset)
                             : n64::kRdramSize4;
    options.timer_interrupt = enable_compare;
    options.ai_delay = enable_fifo_full;
    options.rcp_delays = enable_compare && enable_fifo_full;
    options.fpu_traps = false;
    options.exact_events = false;

    return options;
}

n64::Options TrackOptions(const LoadedUsf& track)
{
    return TrackOptions(track.state, track.enable_compare, track.enable_fifo_full);
}

void StartTrack(n64::System& system, std::span<const uint8_t> rom, std::span<const uint8_t> state)
{
    system.LoadRomImage(std::vector<uint8_t>(rom.begin(), rom.end()));
    system.LoadState(state);
}

uint64_t OutputClock::SliceTicks(uint64_t ticks_per_second)
{
    return static_cast<uint64_t>(kSliceSeconds * static_cast<double>(ticks_per_second));
}

void OutputClock::Start(uint64_t now)
{
    covered_until_ = now;
}

void OutputClock::OnBuffer(uint64_t frames, uint32_t rate, uint64_t now, uint64_t ticks_per_second)
{
    const auto duration =
        static_cast<uint64_t>(static_cast<double>(frames) * static_cast<double>(ticks_per_second) / rate);
    covered_until_ = std::max(covered_until_, now) + duration;
}

uint64_t OutputClock::TakeSilence(uint64_t now, uint32_t rate, uint64_t ticks_per_second)
{
    const auto stall = static_cast<uint64_t>(kStallSeconds * static_cast<double>(ticks_per_second));
    if (now <= covered_until_ + stall)
    {
        return 0;
    }

    const auto frames = static_cast<uint64_t>(
        std::llround(static_cast<double>(now - covered_until_) * rate / static_cast<double>(ticks_per_second)));
    covered_until_ = now;

    return frames;
}

} // namespace usf
