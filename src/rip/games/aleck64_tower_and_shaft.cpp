// SPDX-License-Identifier: MIT

// Tower & Shaft (Aleck64): the game's music, through Software Creations' music library. The game has no sound test.
// Songs go by the game's numbers for them.
//
// Addresses come from the game's code in the image and from runs in usf's core.
//
// - The function at 0x800392F4 takes a song's number, 0 to 18, in a0. It asks for the song only while one of two flags
//   of the game's state is set. It stops the songs that play.
// - The game's scenes call the function at 0x8003919C once a frame. A frame after the request, that function reads the
//   song's file from the cartridge to 0x8011DE60 through the tables at 0x8010EB90 and 0x8010EBDC, and starts it. In
//   every frame it sets the song's volume from the halfword at 0x8009F0F6. The game sets that to 0x78, and its fades
//   count it down.
// - The main thread waits for the next frame in the loop at 0x8002609C. The sample bank loads at 0.56 s. The snapshot
//   takes over the main thread at its first quiet arrival there from 0.61 s on: the 34440th. The volume is 0x78 then.
// - The graphics thread (0x8013A0F8) calls the function at 0x80027190 once a frame. That function reads the board's
//   inputs at 0xE0800000, through a TLB entry that the players refuse. The ripper stops the thread. The music runs in
//   the music library's thread.
// - The graphics thread makes the message queue at 0x8013B0E8 at 0x80086BDC, and adds it to the scheduler's clients at
//   0x80086BEC. At each retrace the scheduler sends a message to its clients, at 0x800855C4, whether the game draws or
//   not. The graphics thread waits for its messages at 0x80086C08, through osRecvMesg at 0x8008E580. The queue is empty
//   at the snapshot. With the graphics thread stopped, the main thread (0x80127410, priority 10) can wait for them.
//   Only the idle thread (0x80127260, priority 0) has a lower priority. Its loop at 0x8007B764 is a branch to itself.
// - Each channel of a song loops separately. At 0x8007EEFC the library has reached the end of a loop, with its count in
//   v1, the channel in a0 and the loop's depth in a2. A count of 0xFF repeats for ever. At 0x8007F0AC the library takes
//   a channel's jump, with the channel in a0. There a2 holds the jump's target. The target is an offset into the
//   channel's script.

#include "rip/driver.h"
#include "rip/games/aleck64.h"
#include "rip/games/games.h"
#include "rip/games/tracks.h"
#include "rip/mips.h"

namespace usf::rip::games
{
namespace
{

constexpr uint32_t kMainLoopWait = 0x8002609C;
constexpr int kWaitAfterBank = 34440;
constexpr uint32_t kAskForSong = 0x800392F4;
constexpr uint32_t kFrameSound = 0x8003919C;
constexpr uint32_t kLoopEnd = 0x8007EEFC;
constexpr uint32_t kGoTo = 0x8007F0AC;
constexpr uint32_t kGraphicsThread = 0x8013A0F8;
constexpr uint32_t kRecvMesg = 0x8008E580;
constexpr uint32_t kGraphicsQueue = 0x8013B0E8;

constexpr uint32_t kSongs = 19;

// The board's inputs at 0xC0800004 while nothing is pressed.
constexpr uint32_t kSystemInputs = 0x003D0000;

} // namespace

GameProfile Aleck64TowerAndShaft()
{
    GameProfile profile = Aleck64Profile(n64::Board::kAleck64,
                                         {
                                             {.sha1 = "7dfa3447d2c489c0448c4004dc12d3037c05a0f3", .size = 0x1000000},
                                         });
    profile.game = "Tower & Shaft";
    profile.serial = "Aleck64 Tower & Shaft";
    profile.sha1 = "ba4474dc4967adb2a41cf3b0022c6215990c0e64";
    profile.year = "2003";
    profile.sound_driver = kSoftwareCreationsLibrary;
    profile.board_inputs.system = kSystemInputs;

    profile.snapshot_address = kMainLoopWait;
    profile.snapshot_hit = kWaitAfterBank;
    profile.quiet_tries = kUntilQuiet;
    profile.boot_seconds = 5.0;
    profile.stop_threads = {kGraphicsThread};

    CallDriverOptions driver;
    driver.address = kAleck64DriverAddress;
    driver.start_calls = {{.function = kAskForSong, .arguments = {{.track = true}}}};
    driver.frame_calls = {{.function = kFrameSound}};
    // Between frames the driver waits for a retrace message on the graphics thread's queue. The idle thread runs
    // meanwhile.
    driver.wait_call = MessageWait(kRecvMesg, kGraphicsQueue);
    profile.driver = BuildCallDriver(driver);

    profile.loop_watches.push_back(
        {.address = kLoopEnd, .target_register = mips::kA2, .track_register = mips::kA0, .count_register = mips::kV1});
    profile.loop_watches.push_back({.address = kGoTo, .target_register = mips::kA2, .track_register = mips::kA0});

    for (uint32_t number = 0; number < kSongs; number++)
    {
        profile.tracks.push_back(NumberedTrack(number, NumberTitle("Song", number, TitleBase::kDecimal)));
    }

    return profile;
}

} // namespace usf::rip::games
