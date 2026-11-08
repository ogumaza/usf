// SPDX-License-Identifier: MIT

// What the profiles of the games of Seta's Aleck64 arcade board share. Each game comes as the board's ROM set. Every
// chip of a set is stored with each pair of bytes reversed. The board has 8 MiB of RDRAM and boots with the
// CIC-NUS-5101. Its inputs are active low: a bit reads 0 while its switch is on or its button is held.

#pragma once

#include <vector>

#include "rip/profile.h"

namespace usf::rip::games
{

// A profile with the board, its RDRAM, its boot chip and the chips of the set. `chips` gives each chip's SHA-1 and size
// in the order of the image, with no gaps.
GameProfile Aleck64Profile(n64::Board board, const std::vector<RomChip>& chips);

} // namespace usf::rip::games
