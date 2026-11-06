// SPDX-License-Identifier: MIT

// The snapshot: the game booted and run to the profile's snapshot point, with the profile's changes made there, and
// the state saved. Each track's state is the snapshot's with the track's number in the selector.

#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "n64/system.h"
#include "rip/profile.h"

namespace usf::rip
{

struct Snapshot
{
    // The ROM in console byte order, with the profile's ROM patches.
    std::vector<uint8_t> rom;

    // The state image at the snapshot.
    std::vector<uint8_t> state;

    // The bytes of the state image that the selector covers: a register's 8 bytes, or the RDRAM word that holds the
    // selector's value. Each mini writes them.
    uint32_t selector_offset = 0;
    uint32_t selector_size = 0;

    // How the number goes into those bytes.
    TrackSelector::Kind selector_kind = TrackSelector::Kind::kRegister;
    uint32_t selector_address = 0; // For RDRAM: the physical address of the value.
    int selector_value_size = 4;

    // When the snapshot was taken, in seconds of emulated time since boot, and which arrival it was.
    double seconds = 0.0;
    int arrival = 0;
};

// Boots `rom` (in any byte order) as `profile` says and takes the snapshot. `pif_rom` may be empty. The timing should
// be the one the rips play with: the legacy players'. Throws std::runtime_error with the reason when the image isn't
// the profile's, the boot fails, the snapshot point isn't reached, or no arrival there is quiet.
Snapshot TakeSnapshot(const GameProfile& profile, std::vector<uint8_t> rom, std::span<const uint8_t> pif_rom,
                      n64::Timing timing = n64::Timing::kLegacy);

// The state image of the track with the number `number`: the snapshot's, with the number in the selector.
std::vector<uint8_t> TrackState(const Snapshot& snapshot, uint32_t number);

// The state image of `track`: as above, with the track's patches of RDRAM written too.
std::vector<uint8_t> TrackState(const Snapshot& snapshot, const Track& track);

// The offset in a state image of the RDRAM word at `address`. Throws std::runtime_error unless the address is
// word-aligned and in KSEG0 or KSEG1.
uint32_t RdramStateOffset(uint32_t address);

// Stops the libultra thread whose OSThread structure is at `thread`, as osStopThread does: takes it off the queue it
// waits in and marks it stopped. Throws std::runtime_error for the running thread or an address the CPU can't reach.
void StopThread(n64::System& system, uint32_t thread);

} // namespace usf::rip
