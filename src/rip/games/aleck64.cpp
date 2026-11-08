// SPDX-License-Identifier: MIT

#include "rip/games/aleck64.h"

namespace usf::rip::games
{

GameProfile Aleck64Profile(n64::Board board, const std::vector<RomChip>& chips)
{
    GameProfile profile;
    profile.board = board;
    profile.rdram_size = n64::kRdramSize8;
    profile.cic = n64::Cic::k5101;

    uint32_t offset = 0;
    for (RomChip chip : chips)
    {
        chip.offset = offset;
        chip.byte_swapped = true;
        offset += chip.size;
        profile.chips.push_back(chip);
    }

    return profile;
}

} // namespace usf::rip::games
