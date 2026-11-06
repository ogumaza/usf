// SPDX-License-Identifier: MIT

// Wave Race 64 (USA, Rev 1): the game's music, played through the sound engine that Super Mario 64 also uses. This is
// the revision that Mario Kart 64 uses, with a queue of commands from the game thread to the audio thread.
//
// Addresses come from the game's code in the image and from runs in usf's core, with a controller and a 4 Kbit EEPROM
// connected. The main segment is copied from cartridge offset 0x1000 to 0x80046800.
//
// - The sequence table at cartridge offset 0x7AE8B0 holds 33 sequences. The audio set-up at 0x800BA37C reads its
//   header and keeps the count at 0x80045514.
// - 0x800C5354 posts a command to the audio thread. 0x800C53B0 and 0x800C538C post one with a word or a float. The
//   send function at 0x800C5404 takes no arguments and sends the posted commands to the audio thread.
// - The music function at 0x800C1504 takes a sequence's number in a0 and a fade-in in a1. It posts command 0x82 with
//   sequence player 1 and the number, which starts the sequence on that player, and sets the player's volume to 0.55
//   (command 0x41).
// - The function at 0x800C13AC starts sequence 0 on sequence player 0, for the sound effects. Sequence 1 (96 bytes)
//   makes no sound on sequence player 1. The other 31 sequences are the tracks.
// - The game first calls 0x800C13AC after the audio reset and before any sound. Right after it, the game calls the
//   music function with sequence 3.
// - The sequence player's script reader handles its jumps (commands 0xF5 and 0xF9 to 0xFB) at 0x800BEBE0 to
//   0x800BEC34. At 0x800BEC30 the new script address is in t3 and the sequence player in s1.

#include <cstdio>

#include "rip/driver.h"
#include "rip/games/games.h"

namespace usf::rip::games
{

namespace
{

constexpr uint32_t kStartEffects = 0x800C13AC;
constexpr uint32_t kPlayMusic = 0x800C1504;
constexpr uint32_t kSendCommands = 0x800C5404;
constexpr uint32_t kSequenceJump = 0x800BEC30;

// The top of RDRAM. Runs of the game hold zeros from 0x803AC400 to 0x80400000.
constexpr uint32_t kDriverAddress = 0x803FF000;

constexpr int kSequences = 33;

} // namespace

GameProfile NusNwreUsaRev1()
{
    GameProfile profile;
    profile.game = "Wave Race 64";
    profile.serial = "NUS-NWRE-USA-rev1";
    profile.sha1 = "508dfc2d4caa42b6f6de5263d0aed5e44ac7966a";
    profile.year = "1996";
    profile.cic = n64::Cic::k6102;
    profile.sound_driver = "Super Mario 64 sound engine";
    profile.controller = true;
    profile.eeprom = n64::EepromType::k4Kbit;

    // The driver takes over the game thread at the first call of the function that starts the sound effects. It
    // plays the track's sequence as the music function does, sends the commands, and then waits.
    profile.snapshot_address = kStartEffects;
    profile.snapshot_hit = 1;
    profile.quiet_tries = 1;
    profile.boot_seconds = 8.0;

    CallDriverOptions driver;
    driver.address = kDriverAddress;
    driver.start_calls = {
        {.function = kPlayMusic, .arguments = {{.track = true}, {.value = 0}}},
        {.function = kSendCommands},
    };
    profile.driver = BuildCallDriver(driver);

    // The sequence players run separate scripts. The player's address in s1 tells their jumps apart.
    profile.loop_watches.push_back({.address = kSequenceJump, .target_register = 11, .track_register = 17});

    for (int index = 2; index < kSequences; index++)
    {
        Track track;
        track.number = static_cast<uint32_t>(index);
        char name[32];
        std::snprintf(name, sizeof(name), "Sequence 0x%02X", index);
        track.name = name;
        profile.tracks.push_back(track);
    }

    return profile;
}

} // namespace usf::rip::games
