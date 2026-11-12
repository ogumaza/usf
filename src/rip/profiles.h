// SPDX-License-Identifier: MIT

// The profiles of the supported games. Each game's profile is made by a function in src/rip/games, declared in games.h
// there and listed in profiles.cpp.

#pragma once

#include <string_view>
#include <vector>

#include "rip/profile.h"

namespace usf::rip
{

// Every supported game's profile.
const std::vector<GameProfile>& Profiles();

// The profiles of every set of the image whose SHA-1 (of its big-endian bytes, in lower-case hex) is `sha1`, in the
// order of Profiles(). Empty when there's none.
std::vector<const GameProfile*> FindProfiles(std::string_view sha1);

} // namespace usf::rip
