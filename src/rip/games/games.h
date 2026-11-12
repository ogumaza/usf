// SPDX-License-Identifier: MIT

// The functions that make each supported game's profile, one source file for each game in this folder, named by the
// image's serial, region and revision (such as nus_ncte_usa_rev1.cpp). A further set of a game may have a file of its
// own, with the set's name added (such as nus_nsme_usa_music.cpp). A game of an arcade board is named by the board and
// the title (such as aleck64_hi_pai_paradise.cpp). profiles.cpp lists them.

#pragma once

#include "rip/profile.h"

namespace usf::rip::games
{

// The sound drivers that more than one profile gives in the usfdriver tag.
inline constexpr const char* kSoftwareCreationsLibrary = "Software Creations' music library";
inline constexpr const char* kCompressedSequencePlayer = "libultra compressed sequence player";
inline constexpr const char* kSuperMario64SoundEngine = "Super Mario 64 sound engine";

GameProfile Aleck64ElevenBeat();
GameProfile Aleck64HanabiDeDoon();
GameProfile Aleck64HiPaiParadise();
GameProfile Aleck64HiPaiParadise2();
GameProfile Aleck64KurukuruFever();
GameProfile Aleck64MagicalTetrisChallenge();
GameProfile Aleck64Mayjinsen3();
GameProfile Aleck64StarSoldier();
GameProfile Aleck64SuperRealMahjongVs();
GameProfile Aleck64SuperRealMahjongVsStereo();
GameProfile Aleck64TowerAndShaft();
GameProfile Aleck64VividDolls();
GameProfile NusCzleUsaRev2();
GameProfile NusN2veUsa();
GameProfile NusNcteUsaRev1();
GameProfile NusNcweUsa();
GameProfile NusNgeeUsa();
GameProfile NusNkteUsa();
GameProfile NusNm8eUsa();
GameProfile NusNmqeUsa();
GameProfile NusNp3eUsa();
GameProfile NusNpfeUsa();
GameProfile NusNpgeUsa();
GameProfile NusNpneUsa();
GameProfile NusNpoeUsaRev2();
GameProfile NusNsmeUsa();
GameProfile NusNsmeUsaMusic();
GameProfile NusNwreUsaRev1();
GameProfile NusNyseUsa();
GameProfile NusNzseUsa();

} // namespace usf::rip::games
