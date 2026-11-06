// SPDX-License-Identifier: MIT

#include "rip/profiles.h"

#include "rip/games/games.h"

namespace usf::rip
{

const std::vector<GameProfile>& Profiles()
{
    // Add each game's profile function here.
    static const std::vector<GameProfile> kProfiles = []
    {
        return std::vector<GameProfile>{
            games::NusCzleUsaRev2(),
            games::NusN2veUsa(),
            games::NusNcteUsaRev1(),
            games::NusNcweUsa(),
            games::NusNgeeUsa(),
            games::NusNkteUsa(),
            games::NusNm8eUsa(),
            games::NusNmqeUsa(),
            games::NusNp3eUsa(),
            games::NusNpfeUsa(),
            games::NusNpgeUsa(),
            games::NusNpneUsa(),
            games::NusNpoeUsaRev2(),
            games::NusNsmeUsa(),
            games::NusNwreUsaRev1(),
            games::NusNzseUsa(),
        };
    }();

    return kProfiles;
}

const GameProfile* FindProfile(std::string_view sha1)
{
    for (const GameProfile& profile : Profiles())
    {
        if (profile.sha1 == sha1)
        {
            return &profile;
        }
    }

    return nullptr;
}

} // namespace usf::rip
