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
            games::Aleck64ElevenBeat(),
            games::Aleck64HanabiDeDoon(),
            games::Aleck64HiPaiParadise(),
            games::Aleck64HiPaiParadise2(),
            games::Aleck64KurukuruFever(),
            games::Aleck64MagicalTetrisChallenge(),
            games::Aleck64Mayjinsen3(),
            games::Aleck64StarSoldier(),
            games::Aleck64SuperRealMahjongVs(),
            games::Aleck64SuperRealMahjongVsStereo(),
            games::Aleck64TowerAndShaft(),
            games::Aleck64VividDolls(),
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
            games::NusNsmeUsaMusic(),
            games::NusNsmeUsa(),
            games::NusNwreUsaRev1(),
            games::NusNyseUsa(),
            games::NusNzseUsa(),
        };
    }();

    return kProfiles;
}

std::vector<const GameProfile*> FindProfiles(std::string_view sha1)
{
    std::vector<const GameProfile*> found;
    for (const GameProfile& profile : Profiles())
    {
        if (profile.sha1 == sha1)
        {
            found.push_back(&profile);
        }
    }

    return found;
}

} // namespace usf::rip
