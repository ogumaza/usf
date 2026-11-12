// SPDX-License-Identifier: MIT

// Magical Tetris Challenge (Aleck64): the game's music, through Software Creations' music library. Songs go by the
// game's numbers for them.
//
// Addresses come from the game's code in the image and from runs in usf's core.
//
// - The function at 0x80006E88 takes a song's number, 0 to 15, in a0. It asks for the song only while one of two flags
//   of the game's state is set. In stereo, the table at 0x8007C310 gives two effects of the music library for each
//   song, one for each side. In mono, the table at 0x8007C490 gives one.
// - The game's main loop calls the function at 0x8000663C once a frame. That function stops the song that plays and
//   starts the first effect, at volume 0x80 and pan 0x80. In stereo it starts the second effect two frames later.
// - Each effect plays one wave on one channel of the library. The game streams the wave from the cartridge through a
//   ring buffer of four blocks of 0x400 bytes. The wave's loop repeats the song. The table of lengths holds where the
//   second pass ends, measured from the cartridge reads of runs in usf's core. Songs 13 and 14 hold no loop. Song 15
//   plays the same waves as song 0, and the set leaves it out.
// - The game's scheduler thread reads the coin inputs once a frame, through the call at 0x800006CC. It reads them at
//   0x02000007, through a TLB entry that the players refuse, and the thread faults. The ripper replaces the call with a
//   nop. The thread of the board's video chip (0x800AC910) writes the chip through other refused TLB entries once a
//   frame, and the input thread (0x800B1EB8) reads the board's controls through one. The ripper stops both threads. The
//   music runs in the music library's thread.
// - The main loop waits for the next frame at 0x8000141C. The snapshot takes over the main loop there, at its 852nd
//   arrival: 16.08 s after boot. The sample banks have loaded by then, and the attract mode asks for song 11 a few
//   frames later.

#include "rip/driver.h"
#include "rip/games/aleck64.h"
#include "rip/games/games.h"
#include "rip/games/tracks.h"
#include "rip/mips.h"

namespace usf::rip::games
{
namespace
{

constexpr uint32_t kMainLoopWait = 0x8000141C;
constexpr int kWaitBeforeSong = 852;
constexpr uint32_t kAskForSong = 0x80006E88;
constexpr uint32_t kFrameSound = 0x8000663C;
constexpr uint32_t kReadCoins = 0x800006CC;
constexpr uint32_t kVideoThread = 0x800AC910;
constexpr uint32_t kInputThread = 0x800B1EB8;

constexpr uint32_t kSongs = 15;

// Where the second pass of each song's waves ends.
constexpr SongLength kSongLengths[] = {
    {0, 137.3}, {1, 117.0}, {2, 22.3},  {3, 11.9},   {4, 20.7},  {5, 18.1},   {6, 18.2},
    {7, 197.5}, {8, 150.7}, {9, 136.7}, {10, 123.0}, {11, 24.9}, {12, 147.1},
};

// The board's inputs at 0xC0800004 while nothing is pressed.
constexpr uint32_t kSystemInputs = 0x0000FFFF;

} // namespace

GameProfile Aleck64MagicalTetrisChallenge()
{
    GameProfile profile = Aleck64Profile(n64::Board::kAleck64E90,
                                         {
                                             {.sha1 = "59932c70b43ff8e9264c670f37b3abbe939b7f95", .size = 0x1000000},
                                         });
    profile.game = "Magical Tetris Challenge";
    profile.serial = "Aleck64 Magical Tetris Challenge";
    profile.sha1 = "7e357995def4078a54ba8e4486f361d631ff3329";
    profile.year = "1998";
    profile.sound_driver = kSoftwareCreationsLibrary;
    profile.board_inputs.system = kSystemInputs;

    profile.snapshot_address = kMainLoopWait;
    profile.snapshot_hit = kWaitBeforeSong;
    profile.quiet_tries = kUntilQuiet;
    profile.boot_seconds = 20.0;
    profile.rdram_patches = {{.address = kReadCoins, .words = {mips::kNop}}};
    profile.stop_threads = {kVideoThread, kInputThread};

    CallDriverOptions driver;
    driver.address = kAleck64DriverAddress;
    driver.start_calls = {{.function = kAskForSong, .arguments = {{.track = true}}}};
    driver.frame_calls = {{.function = kFrameSound}};
    profile.driver = BuildCallDriver(driver);

    for (uint32_t number = 0; number < kSongs; number++)
    {
        profile.tracks.push_back(NumberedTrack(number, NumberTitle("Song", number, TitleBase::kDecimal), kSongLengths));
    }

    return profile;
}

} // namespace usf::rip::games
