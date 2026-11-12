// SPDX-License-Identifier: MIT

// Eleven Beat (Aleck64): the game's music, through Hudson's sound code over libultra's compressed sequence player.
// Songs go by the game's numbers for them.
//
// Addresses come from the game's code in the image and from runs in usf's core.
//
// - The sequence file at cartridge offset 0x36D7D0 holds 20 sequences, in libultra's S2 format. The game reads its
//   table at boot.
// - The function at 0x80004268 takes a song's number in a0. It sets the song's volume from the sequence file and asks
//   for the song. The audio thread reads the song's sequence from the cartridge and starts it once the sequence player
//   has stopped.
// - The attract mode makes no sound. The idle thread loops at 0x80000558. The snapshot takes over the idle thread
//   there, at its first quiet arrival from the 93rd on: 2.01 s after boot.
// - The input thread (0x800B37B0) starts at 0x8000CFA4. Once a frame it calls the function at 0x8000D534. That function
//   reads the board's inputs at 0x02000002, through a TLB entry that the players refuse. The ripper stops the thread.
//   The music runs in the audio thread.
// - The sequence reader at 0x8008FC14 reads one track of a sequence. At 0x8008FDCC it has reached the end of a loop,
//   with the loop's count in a0, the end's address in v0 and the track in t3. A count of 0xFF repeats for ever.

#include "rip/driver.h"
#include "rip/games/aleck64.h"
#include "rip/games/games.h"
#include "rip/games/tracks.h"
#include "rip/mips.h"

namespace usf::rip::games
{
namespace
{

constexpr uint32_t kIdleLoop = 0x80000558;
constexpr int kIdleAfterBoot = 93;
constexpr uint32_t kAskForSong = 0x80004268;
constexpr uint32_t kLoopEnd = 0x8008FDCC;
constexpr uint32_t kInputThread = 0x800B37B0;

constexpr uint32_t kSongs = 20;

// The board's inputs at 0xC0800004 while nothing is pressed.
constexpr uint32_t kSystemInputs = 0xFF3CFFFF;

} // namespace

GameProfile Aleck64ElevenBeat()
{
    GameProfile profile =
        Aleck64Profile(n64::Board::kAleck64, {
                                                 {.sha1 = "824911452639cedf6a8186c05cd046e61fc98896", .size = 0x800000},
                                             });
    profile.game = "Eleven Beat";
    profile.serial = "Aleck64 Eleven Beat";
    profile.sha1 = "5432056f34c378b5862009e5b79a256c10c5a20e";
    profile.year = "1998";
    profile.sound_driver = kCompressedSequencePlayer;
    profile.controller = true;
    profile.board_inputs.system = kSystemInputs;

    profile.snapshot_address = kIdleLoop;
    profile.snapshot_hit = kIdleAfterBoot;
    profile.quiet_tries = kUntilQuiet;
    profile.boot_seconds = 5.0;
    profile.stop_threads = {kInputThread};

    CallDriverOptions driver;
    driver.address = kAleck64DriverAddress;
    driver.start_calls = {{.function = kAskForSong, .arguments = {{.track = true}}}};
    profile.driver = BuildCallDriver(driver);

    profile.loop_watches.push_back(
        {.address = kLoopEnd, .target_register = mips::kV0, .track_register = mips::kT3, .count_register = mips::kA0});

    for (uint32_t number = 0; number < kSongs; number++)
    {
        profile.tracks.push_back(NumberedTrack(number, NumberTitle("Song", number, TitleBase::kDecimal)));
    }

    return profile;
}

} // namespace usf::rip::games
