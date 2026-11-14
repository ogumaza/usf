// SPDX-License-Identifier: MIT

// What the profiles of the games of Seta's Aleck64 arcade board share. Each game comes as the board's ROM set. Every
// chip of a set is stored with each pair of bytes reversed. The board has 8 MiB of RDRAM and boots with the
// CIC-NUS-5101. Its inputs are active low: a bit reads 0 while its switch is on or its button is held.

#pragma once

#include <cstdint>
#include <string_view>

#include "rip/profile.h"

namespace usf::rip::games
{

// RDRAM near the end that stays zero through the attract mode of each game. Each profile places the driver there.
inline constexpr uint32_t kAleck64DriverAddress = 0x807FF000;

// The board's DIP switches as they leave the factory.
inline constexpr uint32_t kFactorySwitches = 0xFFDFFFFF;

// A `quiet_tries` large enough to wait for the first quiet arrival, however late it comes.
inline constexpr int kUntilQuiet = 1000000;

// A profile with the board, its RDRAM, its boot chip, and the game, the image SHA-1 and the chips of the set in
// ultralib's table whose image has `image_sha1`. Throws std::logic_error if the table has no such set.
GameProfile Aleck64Profile(ultralib::machine::Board board, std::string_view image_sha1);

} // namespace usf::rip::games
