// SPDX-License-Identifier: MIT

// Hi Pai Paradise (Aleck64): the game's music, through Software Creations' music library. The game has no sound test.
// Songs go by the game's numbers for them.
//
// Addresses come from the game's code in the image and from runs in usf's core.
//
// - The function at 0x8002E77C takes a song's number, 1 to 13, in a0. The table of halfwords at 0x800918A0 gives the
//   song's file. The function stores the file's number, stops the song that plays and sets a delay. The game's main
//   loop counts the delay down once a frame and then calls the function at 0x8002E870.
// - The function at 0x8002E870 reads the song's file from the cartridge to 0x80204000 through the file table at
//   0x80091CA0, starts it with the sample bank at 0x80200000, and sets its volume. The volume comes from a setting
//   at 0x800918E8: 0xAC by default, or 0xDC. Two values of a setting at 0x80091A0C fade the song out at once.
// - The game's event scripts can also change the volume of the song that plays, through the function at 0x8002E73C.
//   The set plays each song at the volume that the function at 0x8002E870 gives it.
// - The main loop waits for the next frame in the loop at 0x80026740. The snapshot takes over the main loop there, at
//   its first quiet arrival: 1.15 s after boot. At the waits just after a retrace, the audio thread keeps a second
//   audio buffer waiting. The sample bank has loaded by 0.55 s, and the game makes no sound until 12.8 s.
// - The graphics thread (0x800C3440) calls the function at 0x8002709C once a frame. That function reads the board's
//   inputs at 0xE0800000, through a TLB entry that the players refuse. The ripper stops the thread. The music runs in
//   the music library's thread.
// - Each channel of a song loops separately, and some channels start their loop later than others. At 0x8006F7EC the
//   library has reached the end of a loop, with its count in v1, the channel in a0 and the loop's depth in a2. A count
//   of 0xFF repeats for ever. Songs 6, 7 and 13 hold no loop.

#include <cstdio>

#include "rip/driver.h"
#include "rip/games/aleck64.h"
#include "rip/games/games.h"

namespace usf::rip::games
{

namespace
{

constexpr uint32_t kMainLoopFrame = 0x80026750;
constexpr uint32_t kSelectSong = 0x8002E77C;
constexpr uint32_t kStartSong = 0x8002E870;
constexpr uint32_t kLoopEnd = 0x8006F7EC;

constexpr uint32_t kGraphicsThread = 0x800C3440;

// RDRAM near the end that stays zero through the attract mode.
constexpr uint32_t kDriverAddress = 0x807FF000;

constexpr int kSongs = 13;

// Runs of the songs measured where each channel's second pass ends. The table holds the latest of those ends.
struct SongLength
{
    int number;
    double seconds;
};

constexpr SongLength kSongLengths[] = {
    {1, 139.8}, {2, 85.8},  {3, 73.1},   {4, 281.3},  {5, 89.2},
    {8, 65.4},  {9, 113.8}, {10, 188.1}, {11, 225.7}, {12, 90.9},
};

constexpr double kFadeSeconds = 10.0;

// The board's DIP switches as they leave the factory.
constexpr uint32_t kSwitches = 0xFFDFFFFF;

} // namespace

GameProfile Aleck64HiPaiParadise()
{
    GameProfile profile = Aleck64Profile(n64::Board::kAleck64,
                                         {
                                             {.sha1 = "e909ea5b71b81087da07821c4f57244576363678", .size = 0x1000000},
                                             {.sha1 = "7c3e59f6520dc3f0aa592e682fa82e30ffd1f4d0", .size = 0x1000000},
                                         });
    profile.game = "Hi Pai Paradise";
    profile.serial = "Aleck64 Hi Pai Paradise";
    profile.sha1 = "68329c94e1b9342ee60713e9e0945ea2663210c7";
    profile.year = "2003";
    profile.sound_driver = "Software Creations' music library";
    profile.board_inputs.switches = kSwitches;

    profile.snapshot_address = kMainLoopFrame;
    profile.snapshot_hit = 1;
    profile.quiet_tries = 1000000;
    profile.boot_seconds = 5.0;

    profile.stop_threads = {kGraphicsThread};

    CallDriverOptions driver;
    driver.address = kDriverAddress;
    driver.start_calls = {
        {.function = kSelectSong, .arguments = {{.track = true}}},
        {.function = kStartSong},
    };
    profile.driver = BuildCallDriver(driver);

    profile.loop_watches.push_back(
        {.address = kLoopEnd, .target_register = 6, .track_register = 4, .count_register = 3});

    for (int number = 1; number <= kSongs; number++)
    {
        Track track;
        track.number = static_cast<uint32_t>(number);
        char name[16];
        std::snprintf(name, sizeof(name), "Song %d", number);
        track.name = name;
        for (const SongLength& length : kSongLengths)
        {
            if (length.number == number)
            {
                track.length_seconds = length.seconds;
                track.fade_seconds = kFadeSeconds;
            }
        }
        profile.tracks.push_back(track);
    }

    return profile;
}

} // namespace usf::rip::games
