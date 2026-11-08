// SPDX-License-Identifier: MIT

// Mayjinsen 3 (Aleck64): the game's music, through libultra's compressed sequence player. Songs go by the numbers of
// the sequences.
//
// Addresses come from the game's code in the image and from runs in usf's core.
//
// - The sequence file at cartridge offset 0x600660 holds 12 sequences. The game reads it to 0x8008BEE0 at boot.
// - The function at 0x8002877C takes a sequence's number in a0. It stops the song that plays, reads the sequence from
//   the cartridge and starts it.
// - The attract mode makes no sound. The idle thread loops at 0x80009FC8. The snapshot takes over the idle thread
//   there, at its first quiet arrival from the 7468372nd on: 5.03 s after boot.
// - The thread at 0x8006F930 calls the function at 0x80026BF8 once a frame. That function reads the board's inputs at
//   0x02000002, through a TLB entry that the players refuse. The ripper stops the thread. The music runs in the audio
//   thread.
// - The sequence reader at 0x80031A80 reads one track of a sequence. At 0x80031BDC it has reached the end of a loop,
//   with the loop's count in v1, the end's address in a2 and the track in s2. A count of 0xFF repeats for ever.

#include <cstdio>

#include "rip/driver.h"
#include "rip/games/aleck64.h"
#include "rip/games/games.h"

namespace usf::rip::games
{

namespace
{

constexpr uint32_t kIdleLoop = 0x80009FC8;
constexpr int kIdleAfterBoot = 7468372;
constexpr uint32_t kPlaySequence = 0x8002877C;
constexpr uint32_t kLoopEnd = 0x80031BDC;
constexpr uint32_t kBoardThread = 0x8006F930;

// RDRAM near the end that stays zero through the attract mode.
constexpr uint32_t kDriverAddress = 0x807FF000;

constexpr int kSequences = 12;

} // namespace

GameProfile Aleck64Mayjinsen3()
{
    GameProfile profile =
        Aleck64Profile(n64::Board::kAleck64, {
                                                 {.sha1 = "b5834bfde5b8a7e20415b2593abd76ec95ab27c7", .size = 0x800000},
                                             });
    profile.game = "Mayjinsen 3";
    profile.serial = "Aleck64 Mayjinsen 3";
    profile.sha1 = "da401fc96a2b69db81970d879a2255e54db88e34";
    profile.year = "2000";
    profile.sound_driver = "libultra compressed sequence player";

    profile.snapshot_address = kIdleLoop;
    profile.snapshot_hit = kIdleAfterBoot;
    profile.quiet_tries = 1000000;
    profile.boot_seconds = 10.0;
    profile.stop_threads = {kBoardThread};

    CallDriverOptions driver;
    driver.address = kDriverAddress;
    driver.start_calls = {{.function = kPlaySequence, .arguments = {{.track = true}}}};
    profile.driver = BuildCallDriver(driver);

    profile.loop_watches.push_back(
        {.address = kLoopEnd, .target_register = 6, .track_register = 18, .count_register = 3});

    for (int number = 0; number < kSequences; number++)
    {
        Track track;
        track.number = static_cast<uint32_t>(number);
        char name[16];
        std::snprintf(name, sizeof(name), "Sequence %d", number);
        track.name = name;
        profile.tracks.push_back(track);
    }

    return profile;
}

} // namespace usf::rip::games
