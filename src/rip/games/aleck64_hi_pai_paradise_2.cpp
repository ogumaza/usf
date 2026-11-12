// SPDX-License-Identifier: MIT

// Hi Pai Paradise 2 (Aleck64): the game's music, through Software Creations' music library. The game has no sound test.
// Songs go by the game's numbers for them.
//
// Addresses come from the game's code in the image and from runs in usf's core.
//
// - The function at 0x8002E9AC takes a song's number, 1 to 18, in a0. The table of halfwords at 0x80095680 gives the
//   song's file. The function stores the file's number, stops the song that plays and sets a delay. The game's main
//   loop counts the delay down once a frame and then calls the function at 0x8002EAA0.
// - The function at 0x8002EAA0 reads the song's file from the cartridge to 0x80204000 through the file table at
//   0x80095B50, starts it with the sample bank at 0x80200000, and sets its volume. The volume comes from a setting at
//   0x800956C8, and both of its values give 0xE6. Two values of a setting at 0x800957FC fade the song out at once.
// - The game's event scripts can also change the volume of the song that plays, through the function at 0x8002E96C. The
//   set plays each song at the volume that the function at 0x8002EAA0 gives it.
// - The main loop waits for the next frame in the loop at 0x80026738. The snapshot takes over the main loop there, at
//   its first quiet arrival: 0.68 s after boot. At the waits just after a retrace, the audio thread keeps a second
//   audio buffer waiting. The sample bank has loaded by then.
// - The graphics thread (0x800C86A0) reads the board's inputs once a frame, at 0xE0800000, through a TLB entry that the
//   players refuse. The ripper stops the thread. The music runs in the music library's thread.
// - Each channel of a song loops separately. At 0x8007368C the library has reached the end of a loop, with its count in
//   v1, the channel in a0 and the loop's depth in a2. A count of 0xFF repeats for ever. At 0x8007383C the library takes
//   a channel's jump, with the channel in a0. There a2 holds the jump's target. The target is an offset into the
//   channel's script. Songs 4, 12, 13, 14, 17 and 18 hold no loop. Song 7 plays for 7.6 s and then holds 44 s of
//   silence before its loop. Its length ends where its second pass stops sounding.
// - Numbers 2 and 11 play the same song. The set leaves out 11.

#include "rip/driver.h"
#include "rip/games/aleck64.h"
#include "rip/games/games.h"
#include "rip/games/tracks.h"
#include "rip/mips.h"

namespace usf::rip::games
{
namespace
{

constexpr uint32_t kMainLoopWait = 0x80026748;
constexpr uint32_t kSelectSong = 0x8002E9AC;
constexpr uint32_t kStartSong = 0x8002EAA0;
constexpr uint32_t kLoopEnd = 0x8007368C;
constexpr uint32_t kGoTo = 0x8007383C;

constexpr uint32_t kGraphicsThread = 0x800C86A0;

constexpr uint32_t kSongs = 18;
constexpr uint32_t kSameAsSong2 = 11;

// Runs of the songs measured where each channel's second pass ends. The table holds the latest of those ends.
constexpr SongLength kSongLengths[] = {
    {1, 96.0}, {2, 135.5}, {3, 104.1}, {5, 103.0},  {6, 96.0},   {7, 59.7},
    {8, 48.0}, {9, 136.9}, {10, 64.0}, {15, 119.9}, {16, 101.1},
};

} // namespace

GameProfile Aleck64HiPaiParadise2()
{
    GameProfile profile = Aleck64Profile(n64::Board::kAleck64,
                                         {
                                             {.sha1 = "279a006436069232383b2065e7416d8ccc76e3ca", .size = 0x1000000},
                                             {.sha1 = "a6d2d805f96f0ad7294ebf41b13573320154b2a4", .size = 0x1000000},
                                         });
    profile.game = "Hi Pai Paradise 2";
    profile.serial = "Aleck64 Hi Pai Paradise 2";
    profile.sha1 = "47fdc89620ddf68972e861dee1536a26a309e35c";
    profile.year = "2004";
    profile.sound_driver = kSoftwareCreationsLibrary;
    profile.board_inputs.switches = kFactorySwitches;

    profile.snapshot_address = kMainLoopWait;
    profile.snapshot_hit = 1;
    profile.quiet_tries = kUntilQuiet;
    profile.boot_seconds = 5.0;

    profile.stop_threads = {kGraphicsThread};

    CallDriverOptions driver;
    driver.address = kAleck64DriverAddress;
    driver.start_calls = {
        {.function = kSelectSong, .arguments = {{.track = true}}},
        {.function = kStartSong},
    };
    profile.driver = BuildCallDriver(driver);

    profile.loop_watches.push_back(
        {.address = kLoopEnd, .target_register = mips::kA2, .track_register = mips::kA0, .count_register = mips::kV1});
    profile.loop_watches.push_back({.address = kGoTo, .target_register = mips::kA2, .track_register = mips::kA0});

    for (uint32_t number = 1; number <= kSongs; number++)
    {
        if (number == kSameAsSong2)
        {
            continue;
        }

        profile.tracks.push_back(NumberedTrack(number, NumberTitle("Song", number, TitleBase::kDecimal), kSongLengths));
    }

    return profile;
}

} // namespace usf::rip::games
