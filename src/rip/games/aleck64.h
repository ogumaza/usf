// SPDX-License-Identifier: MIT

// What the profiles of the games of Seta's Aleck64 arcade board share. Each game comes as the board's ROM set. Every
// chip of a set is stored with each pair of bytes reversed. The board has 8 MiB of RDRAM and boots with the
// CIC-NUS-5101. Its inputs are active low: a bit reads 0 while its switch is on or its button is held.

#pragma once

#include <cstdint>
#include <vector>

#include "rip/profile.h"

namespace usf::rip::games
{

// RDRAM near the end that stays zero through the attract mode of each game. Each profile places the driver there.
inline constexpr uint32_t kAleck64DriverAddress = 0x807FF000;

// The board's DIP switches as they leave the factory.
inline constexpr uint32_t kFactorySwitches = 0xFFDFFFFF;

// A `quiet_tries` large enough to wait for the first quiet arrival, however late it comes.
inline constexpr int kUntilQuiet = 1000000;

// A profile with the board, its RDRAM, its boot chip and the chips of the set. `chips` gives each chip's SHA-1 and size
// in the order of the image, with no gaps.
GameProfile Aleck64Profile(n64::Board board, const std::vector<RomChip>& chips);

} // namespace usf::rip::games
