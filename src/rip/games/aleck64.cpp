// SPDX-License-Identifier: MIT

#include "rip/games/aleck64.h"

#include <stdexcept>
#include <string>

#include "ultralib/machine/rom_set.h"

namespace usf::rip::games
{

GameProfile Aleck64Profile(ultralib::machine::Board board, std::string_view image_sha1)
{
    const ultralib::machine::Aleck64Set* set = ultralib::machine::FindAleck64Set(image_sha1);
    if (set == nullptr)
    {
        throw std::logic_error("ultralib has no Aleck64 set with the image SHA-1 " + std::string(image_sha1));
    }

    GameProfile profile;
    profile.game = set->name;
    profile.sha1 = set->image_sha1;
    profile.chips.assign(set->chips.begin(), set->chips.end());
    profile.board = board;
    profile.rdram_size = ultralib::machine::kRdramSize8;
    profile.cic = ultralib::machine::Cic::k5101;

    return profile;
}

} // namespace usf::rip::games
