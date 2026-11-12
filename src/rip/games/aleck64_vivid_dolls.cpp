// SPDX-License-Identifier: MIT

// Vivid Dolls (Aleck64): the game's music, through libultra's compressed sequence player. Songs go by the numbers of
// the sequences.
//
// Addresses come from the game's code in the image and from runs in usf's core.
//
// - The sequence file at cartridge offset 0x1D7720 holds 15 sequences.
// - The function at 0x8000D1CC takes a sequence's number in a0. It stops the song that plays, reads the sequence from
//   the cartridge and starts it. The game's function at 0x8001BBC0 plays songs 0 to 5 as sequences 0 to 5, and finds
//   the sequences of the others in the table at 0x8003A240.
// - The game reads the sound banks and the sequence file at 1.08 s. The attract mode makes no sound. The thread that
//   starts at 0x8000BD28 waits for its messages at 0x8000BDF8. The snapshot takes over that thread there, at its first
//   quiet arrival from the 187th on, about 2.75 s after boot. The function at 0x8000D1CC waits for the cartridge, and
//   the idle thread can't wait.
// - In its loop, the thread at 0x8003E2E8 calls the function at 0x8000FA4C. That function reads the board's inputs at
//   0x02000004, through a TLB entry that the players refuse. The ripper stops the thread. The music runs in the audio
//   thread.
// - The sequence reader at 0x8001DB30 reads one track of a sequence. At 0x8001DC8C it has reached the end of a loop,
//   with the loop's count in v1, the end's address in a2 and the track in s2. A count of 0xFF repeats for ever.

#include "rip/driver.h"
#include "rip/games/aleck64.h"
#include "rip/games/games.h"
#include "rip/games/tracks.h"
#include "rip/mips.h"

namespace usf::rip::games
{
namespace
{

constexpr uint32_t kMessageWait = 0x8000BDF8;
constexpr int kWaitAfterBoot = 187;
constexpr uint32_t kPlaySequence = 0x8000D1CC;
constexpr uint32_t kLoopEnd = 0x8001DC8C;
constexpr uint32_t kInputThread = 0x8003E2E8;

constexpr uint32_t kSequences = 15;

} // namespace

GameProfile Aleck64VividDolls()
{
    GameProfile profile =
        Aleck64Profile(n64::Board::kAleck64, {
                                                 {.sha1 = "06d8b808cc19378b046803f4dc75c7d791b7767f", .size = 0x800000},
                                             });
    profile.game = "Vivid Dolls";
    profile.serial = "Aleck64 Vivid Dolls";
    profile.sha1 = "747e9420cce23ff82b5922cd91ec43272d03fa4f";
    profile.year = "1998";
    profile.sound_driver = kCompressedSequencePlayer;

    profile.snapshot_address = kMessageWait;
    profile.snapshot_hit = kWaitAfterBoot;
    profile.quiet_tries = kUntilQuiet;
    profile.boot_seconds = 10.0;
    profile.stop_threads = {kInputThread};

    CallDriverOptions driver;
    driver.address = kAleck64DriverAddress;
    driver.start_calls = {{.function = kPlaySequence, .arguments = {{.track = true}}}};
    profile.driver = BuildCallDriver(driver);

    profile.loop_watches.push_back(
        {.address = kLoopEnd, .target_register = mips::kA2, .track_register = mips::kS2, .count_register = mips::kV1});

    for (uint32_t number = 0; number < kSequences; number++)
    {
        profile.tracks.push_back(NumberedTrack(number, NumberTitle("Sequence", number, TitleBase::kDecimal)));
    }

    return profile;
}

} // namespace usf::rip::games
