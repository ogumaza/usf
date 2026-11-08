// SPDX-License-Identifier: MIT

#include "rip/rom_set.h"

#include <cstdint>
#include <set>
#include <utility>
#include <vector>

namespace usf::rip
{

std::optional<std::string> ReadRomSet(const std::filesystem::path& path, std::span<const GameProfile> profiles,
                                      std::vector<SetFile>& files)
{
    std::set<uint64_t> sizes;
    for (const GameProfile& profile : profiles)
    {
        for (const RomChip& chip : profile.chips)
        {
            sizes.insert(chip.size);
        }
    }

    return xsflib::rip::ReadRomSet(path, sizes, "usfrip", files);
}

RomSetImage BuildRomSetImage(std::span<const GameProfile> profiles, const std::vector<SetFile>& files)
{
    std::vector<xsflib::rip::RomSetGame> games;
    games.reserve(profiles.size());
    for (const GameProfile& profile : profiles)
    {
        games.push_back({profile.game, profile.chips});
    }

    xsflib::rip::RomSetImage image = xsflib::rip::BuildRomSetImage(games, files);
    RomSetImage result;
    if (image.game)
    {
        result.profile = &profiles[*image.game];
    }
    result.rom = std::move(image.rom);
    result.problem = std::move(image.problem);

    return result;
}

} // namespace usf::rip
