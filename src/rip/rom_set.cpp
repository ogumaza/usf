// SPDX-License-Identifier: MIT

#include "rip/rom_set.h"

#include <algorithm>
#include <cstdint>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/sha1.h"

namespace usf::rip
{
namespace
{

// The games of `profiles`, in their order, for xsflib's loader.
std::vector<xsflib::rip::RomSetGame> Games(std::span<const GameProfile> profiles)
{
    std::vector<xsflib::rip::RomSetGame> games;
    games.reserve(profiles.size());
    for (const GameProfile& profile : profiles)
    {
        games.push_back({profile.game, profile.chips});
    }

    return games;
}

// The first profile of each game whose chips are all among `files`, other than the game of `found`. Profiles that share
// an image SHA-1 are sets of one game.
std::vector<const GameProfile*> OtherCompleteGames(std::span<const GameProfile> profiles,
                                                   const std::vector<SetFile>& files, const GameProfile& found)
{
    std::set<std::pair<uint64_t, std::string>> chips;
    for (const SetFile& file : files)
    {
        chips.emplace(file.data.size(), xsflib::Sha1::Hex(xsflib::Sha1::Of(file.data)));
    }

    std::vector<const GameProfile*> others;
    std::set<std::string> images = {found.sha1};
    for (const GameProfile& profile : profiles)
    {
        if (profile.chips.empty() || images.count(profile.sha1) != 0)
        {
            continue;
        }

        const auto here = [&](const RomChip& chip)
        {
            return chips.count({chip.size, chip.sha1}) != 0;
        };
        if (std::all_of(profile.chips.begin(), profile.chips.end(), here))
        {
            others.push_back(&profile);
            images.insert(profile.sha1);
        }
    }

    return others;
}

} // namespace

std::optional<std::string> ReadRomSet(const std::filesystem::path& path, std::span<const GameProfile> profiles,
                                      std::vector<SetFile>& files)
{
    return xsflib::rip::ReadRomSet(path, Games(profiles), "usfrip", files);
}

RomSetImage BuildRomSetImage(std::span<const GameProfile> profiles, const std::vector<SetFile>& files)
{
    xsflib::rip::RomSetImage image = xsflib::rip::BuildRomSetImage(Games(profiles), files);
    RomSetImage result;
    if (image.game)
    {
        result.profile = &profiles[*image.game];
    }
    result.rom = std::move(image.rom);
    result.problem = std::move(image.problem);

    if (result.profile != nullptr)
    {
        result.others = OtherCompleteGames(profiles, files, *result.profile);
    }

    return result;
}

} // namespace usf::rip
