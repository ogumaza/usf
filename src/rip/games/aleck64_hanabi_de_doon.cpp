// SPDX-License-Identifier: MIT

// Hanabi de Doon! - Don-chan Puzzle (Aleck64): the game's music, through Software Creations' music library. The game
// has no sound test. Songs go by the game's numbers for them.
//
// Addresses come from the game's code in the image and from runs in usf's core. The game's code is copied from
// cartridge offset 0x30A70 to 0x8012AE70.
//
// - The function at 0x8014009C takes a song's number in a0. It asks for the song only while one of two flags of the
//   game's state is set. The graphics thread calls the function at 0x801401BC once a frame. That function finds the
//   song's number in the table at 0x80166A00, stops the song that plays and starts the song as an effect of the music
//   library, at volume 0x7F and pan 0x80. It then sets the effect's volume to 0xE7.
// - The game's logic runs in the graphics thread (0x800A6BA8), and the main thread waits in the loop at 0x80025F2C. The
//   snapshot takes over the main thread there, 0.84 s after boot, and stops the graphics thread. The sound banks have
//   loaded by then.
// - Each song is one wave on one channel of the library. The library streams the wave from the cartridge, and the
//   wave's loop repeats the song. The table of lengths holds where the second pass ends, measured from the
//   cartridge reads of runs in usf's core. Songs 0x0C and 0x17 to 0x1B hold no loop. Song 0x0F ends in a short loop of
//   silence.

#include <cstdio>

#include "rip/driver.h"
#include "rip/games/aleck64.h"
#include "rip/games/games.h"

namespace usf::rip::games
{

namespace
{

constexpr uint32_t kMainThreadWait = 0x80025F2C;
constexpr uint32_t kAskForSong = 0x8014009C;
constexpr uint32_t kFrameSound = 0x801401BC;
constexpr uint32_t kGraphicsThread = 0x800A6BA8;

// RDRAM near the end that stays zero through the attract mode.
constexpr uint32_t kDriverAddress = 0x807FF000;

// The numbers in the table at 0x80166A00.
constexpr int kSongs[] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A,
                          0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x17, 0x18, 0x19, 0x1A, 0x1B};

// Where the second pass of each song's wave ends.
struct SongLength
{
    int number;
    double seconds;
};

constexpr SongLength kSongLengths[] = {
    {0x00, 25.7}, {0x01, 44.3}, {0x02, 57.1}, {0x03, 57.0}, {0x04, 83.1}, {0x05, 64.1}, {0x06, 66.5},
    {0x07, 71.2}, {0x08, 62.3}, {0x09, 31.8}, {0x0A, 70.1}, {0x0B, 27.5}, {0x0D, 19.7}, {0x0E, 14.2},
};

constexpr double kFadeSeconds = 10.0;

} // namespace

GameProfile Aleck64HanabiDeDoon()
{
    GameProfile profile = Aleck64Profile(n64::Board::kAleck64,
                                         {
                                             {.sha1 = "4f41ee23edc18110be1218ba333d1c58376ab175", .size = 0x1000000},
                                             {.sha1 = "c8cc6c0a456b593aef711d0a75b2342ba2f8203f", .size = 0x1000000},
                                         });
    profile.game = "Hanabi de Doon! - Don-chan Puzzle";
    profile.serial = "Aleck64 Hanabi de Doon";
    profile.sha1 = "2a77e47d34b8d9d89eb37ffc062e9ec0c2121abc";
    profile.year = "2003";
    profile.sound_driver = "Software Creations' music library";

    profile.snapshot_address = kMainThreadWait;
    profile.snapshot_hit = 1;
    profile.quiet_tries = 1000000;
    profile.boot_seconds = 5.0;
    profile.stop_threads = {kGraphicsThread};

    CallDriverOptions driver;
    driver.address = kDriverAddress;
    driver.start_calls = {{.function = kAskForSong, .arguments = {{.track = true}, {.value = 1}, {.value = 0}}}};
    driver.frame_calls = {{.function = kFrameSound}};
    profile.driver = BuildCallDriver(driver);

    for (const int number : kSongs)
    {
        Track track;
        track.number = static_cast<uint32_t>(number);
        char name[16];
        std::snprintf(name, sizeof(name), "Song 0x%02X", number);
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
