// SPDX-License-Identifier: MIT

// Super Real Mahjong VS (Aleck64): the game's music, through libultra's compressed sequence player. Songs go by the
// game's numbers for them. The game plays its music in mono or in stereo, and each mode is a set: "mono", the mode
// that the game starts in, and "stereo".
//
// Addresses come from the game's code in the image and from runs in usf's core.
//
// - The sequence file at cartridge offset 0x268770 holds 40 sequences. The first 20 play in stereo, and the next 20
//   hold the same songs for mono. Sequences 13 to 19 are empty, and so are 33 to 39.
// - The function at 0x80030158 takes a song's number in a0. It asks for the song only while the flag at 0x800E3F0D is
//   set. It calls the function at 0x80001E38, which stops the song that plays, reads the song's sequence from the
//   cartridge and starts it. In mono, the word at 0x80085860 holds 1, and that function adds 20 to the song's number.
// - A setting of the service menu picks the mode: bit 0 of the byte at 0x800DE6C8, among the game's saved settings,
//   which the function at 0x80005058 returns. At boot, the code at 0x800337A8 plays stereo when the bit is set. The
//   function at 0x80005388 clears the settings when their checksum fails, which leaves the game in mono. The menu's
//   code at 0x80033B0C reads the setting, and its change calls the function at 0x80001CE0 with the new mode: 0 for
//   stereo. That function sets the word at 0x80085860 and gives the player the mode's bank file: the one at cartridge
//   offset 0x81F90 for mono, or the one at 0x8E040 for stereo. The stereo set's driver makes the same call before it
//   asks for the song.
// - The function at 0x80035DB4 unpacks run-length data. The attract mode asks for song 12 at 4.21 s and then unpacks
//   the data of its next screen. The snapshot takes over the game's thread in the unpacking, at its first quiet arrival
//   at 0x80035DEC from 4.21 s on: 4.30 s after boot.
// - The thread of the game's main loop (0x8008A9E0) calls the function at 0x8002EDD8 once a frame. That function reads
//   the board's inputs at 0xE0800004, through a TLB entry that the players refuse. The ripper stops the thread. The
//   music runs in the audio thread.
// - The sequence reader at 0x8003E3A0 reads one track of a sequence. At 0x8003E4FC it has reached the end of a loop,
//   with the loop's count in v1, the end's address in a2 and the track in s2. A count of 0xFF repeats for ever.

#include <cstdio>

#include "rip/driver.h"
#include "rip/games/aleck64.h"
#include "rip/games/games.h"

namespace usf::rip::games
{

namespace
{

constexpr uint32_t kUnpackLoop = 0x80035DEC;
constexpr int kUnpackAfterBoot = 11502;
constexpr uint32_t kPlaySong = 0x80030158;
constexpr uint32_t kSetMode = 0x80001CE0;
constexpr uint32_t kStereoMode = 0;
constexpr uint32_t kLoopEnd = 0x8003E4FC;
constexpr uint32_t kMainLoopThread = 0x8008A9E0;

// RDRAM near the end that stays zero through the attract mode.
constexpr uint32_t kDriverAddress = 0x807FF000;

constexpr int kSongs = 13;

// The board's DIP switches as they leave the factory.
constexpr uint32_t kSwitches = 0xFFDFFFFF;

// The set of the mode that `stereo` picks.
GameProfile Aleck64SuperRealMahjongVsSet(bool stereo)
{
    GameProfile profile = Aleck64Profile(n64::Board::kAleck64,
                                         {
                                             {.sha1 = "a78de955f2fcd99dda14e782984368b320eb5415", .size = 0x2000000},
                                         });
    profile.game = "Super Real Mahjong VS";
    profile.serial = "Aleck64 Super Real Mahjong VS";
    profile.set = stereo ? "stereo" : "mono";
    profile.sha1 = "a590216a171a61517cd07ff629d8215631c8be87";
    profile.year = "1999";
    profile.sound_driver = "libultra compressed sequence player";
    profile.board_inputs.switches = kSwitches;

    profile.snapshot_address = kUnpackLoop;
    profile.snapshot_hit = kUnpackAfterBoot;
    profile.quiet_tries = 1000000;
    profile.boot_seconds = 10.0;
    profile.stop_threads = {kMainLoopThread};

    CallDriverOptions driver;
    driver.address = kDriverAddress;
    if (stereo)
    {
        driver.start_calls.push_back({.function = kSetMode, .arguments = {{.value = kStereoMode}}});
    }
    driver.start_calls.push_back({.function = kPlaySong, .arguments = {{.track = true}}});
    profile.driver = BuildCallDriver(driver);

    profile.loop_watches.push_back(
        {.address = kLoopEnd, .target_register = 6, .track_register = 18, .count_register = 3});

    for (int number = 0; number < kSongs; number++)
    {
        Track track;
        track.number = static_cast<uint32_t>(number);
        char name[16];
        std::snprintf(name, sizeof(name), "Song %d", number);
        track.name = name;
        profile.tracks.push_back(track);
    }

    return profile;
}

} // namespace

GameProfile Aleck64SuperRealMahjongVs()
{
    return Aleck64SuperRealMahjongVsSet(false);
}

GameProfile Aleck64SuperRealMahjongVsStereo()
{
    return Aleck64SuperRealMahjongVsSet(true);
}

} // namespace usf::rip::games
