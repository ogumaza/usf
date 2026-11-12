// SPDX-License-Identifier: MIT

// Kurukuru Fever (Aleck64): the game's music, through Software Creations' music library. The game has no sound test.
// Songs go by the game's numbers for them.
//
// Addresses come from the game's code in the image and from runs in usf's core. The game's code is copied from
// cartridge offset 0x30770 to 0x8012AA70.
//
// - The function at 0x8013E8AC takes a song's number in a0. It asks for the song only while one of two flags of the
//   game's state is set. The graphics thread calls the function at 0x8013E9F4 once a frame. That function finds the
//   song's number in the table at 0x80160200, stops the song that plays and starts the song as an effect of the music
//   library, at volume 0x7F and pan 0x80. Numbers 0x12 and 0x16 start the same song. The set leaves out 0x16.
// - The game's logic runs in the graphics thread (0x800A67A8), and the main thread waits in the loop at 0x80025F2C. The
//   snapshot takes over the main thread there, 0.88 s after boot, and stops the graphics thread. The sound banks have
//   loaded by then.
// - Each song is one wave on one channel of the library. The library streams the wave from the cartridge, and the
//   wave's loop repeats the song. The table of lengths holds where the second pass ends, measured from the cartridge
//   reads of runs in usf's core. Songs 0x09, 0x0D to 0x0F, 0x12, 0x14, 0x15 and 0x17 to 0x19 hold no loop.

#include "rip/driver.h"
#include "rip/games/aleck64.h"
#include "rip/games/games.h"
#include "rip/games/tracks.h"

namespace usf::rip::games
{
namespace
{

constexpr uint32_t kMainThreadWait = 0x80025F2C;
constexpr uint32_t kAskForSong = 0x8013E8AC;
constexpr uint32_t kFrameSound = 0x8013E9F4;
constexpr uint32_t kGraphicsThread = 0x800A67A8;

// The numbers in the table at 0x80160200, but 0x16.
constexpr uint32_t kSongs[] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C,
                               0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x17, 0x18, 0x19};

// Where the second pass of each song's wave ends.
constexpr SongLength kSongLengths[] = {
    {0x00, 137.5}, {0x01, 106.3}, {0x02, 193.2}, {0x03, 159.5}, {0x04, 159.5},
    {0x05, 119.6}, {0x06, 149.5}, {0x07, 122.1}, {0x08, 93.8},  {0x0A, 33.3},
    {0x0B, 79.8},  {0x0C, 62.3},  {0x10, 33.2},  {0x11, 27.7},  {0x13, 14.2},
};

} // namespace

GameProfile Aleck64KurukuruFever()
{
    GameProfile profile = Aleck64Profile(n64::Board::kAleck64,
                                         {
                                             {.sha1 = "824fdce01fffdfcbcc9b1fbda4ab389a10b2b418", .size = 0x1000000},
                                             {.sha1 = "2b6ca1a769dee74e112c2b287dacd0bf46dda091", .size = 0x1000000},
                                         });
    profile.game = "Kurukuru Fever";
    profile.serial = "Aleck64 Kurukuru Fever";
    profile.sha1 = "f59c121e4d4c2c57c2ea4728919775a8d5d5e02e";
    profile.year = "2003";
    profile.sound_driver = kSoftwareCreationsLibrary;

    profile.snapshot_address = kMainThreadWait;
    profile.snapshot_hit = 1;
    profile.quiet_tries = kUntilQuiet;
    profile.boot_seconds = 5.0;
    profile.stop_threads = {kGraphicsThread};

    CallDriverOptions driver;
    driver.address = kAleck64DriverAddress;
    // The request passes 1 in a1 and 0 in a2. So does the game's request at 0x8014A1C8. These values set a word that
    // only the function at 0x8013E870 reads. That function returns the song asked for.
    driver.start_calls = {{.function = kAskForSong, .arguments = {{.track = true}, {.value = 1}, {.value = 0}}}};
    driver.frame_calls = {{.function = kFrameSound}};
    profile.driver = BuildCallDriver(driver);

    for (const uint32_t number : kSongs)
    {
        profile.tracks.push_back(NumberedTrack(number, NumberTitle("Song", number, TitleBase::kHex), kSongLengths));
    }

    return profile;
}

} // namespace usf::rip::games
