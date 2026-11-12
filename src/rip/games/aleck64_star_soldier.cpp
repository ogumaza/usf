// SPDX-License-Identifier: MIT

// Star Soldier: Vanishing Earth (Aleck64): the game's music, through Hudson's sound code over libultra's compressed
// sequence player. Songs go by the game's numbers for them.
//
// Addresses come from the game's code in the image and from runs in usf's core.
//
// - The sequence file at cartridge offset 0x9B180 holds 26 sequences, in libultra's S2 format. The game reads it to
//   0x803B8880 at boot. Sequence 19 is the same as sequence 15, and the set leaves it out.
// - The function at 0x80003100 takes a song's number in a0. It sets the song's volume from the sequence file and asks
//   for the song. The audio thread calls the function at 0x80002E58 in each audio frame. Once the sequence player has
//   stopped, that function reads the song's sequence from the cartridge and starts it.
// - The game's main loop waits for the next frame at 0x800568A8. The snapshot takes over the game's thread there, at
//   its first quiet arrival from the 9th on: 1.60 s after boot. The game has reset its sound by then, and the attract
//   mode makes no sound until 19.9 s.
// - The game needs a controller to start the attract mode.
// - Two threads read the board through TLB entries that the players refuse: the thread at 0x800A8CE0 at 0x0200000A, and
//   the thread at 0x800ABFC0 at 0x02000002. The ripper stops both. The music runs in the audio thread.
// - The sequence reader at 0x8005AFC4 reads one track of a sequence. At 0x8005B17C it has reached the end of a loop,
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

constexpr uint32_t kMainLoopWait = 0x800568A8;
constexpr int kWaitAfterReset = 9;
constexpr uint32_t kAskForSong = 0x80003100;
constexpr uint32_t kLoopEnd = 0x8005B17C;
constexpr uint32_t kFirstBoardThread = 0x800A8CE0;
constexpr uint32_t kSecondBoardThread = 0x800ABFC0;

constexpr uint32_t kSongs = 26;
constexpr uint32_t kSameAsSong15 = 19;

// The board's inputs while nothing is pressed.
constexpr uint32_t kSwitches = 0x7FDFFFFF;
constexpr uint32_t kSystemInputs = 0xFF3CFFFF;

} // namespace

GameProfile Aleck64StarSoldier()
{
    GameProfile profile =
        Aleck64Profile(n64::Board::kAleck64, {
                                                 {.sha1 = "3794606c008fb69f5d16dcccece94d03da23bf8a", .size = 0xC00000},
                                             });
    profile.game = "Star Soldier: Vanishing Earth";
    profile.serial = "Aleck64 Star Soldier: Vanishing Earth";
    profile.sha1 = "a29348632ed7726d75d693dfb007f277c1f6a563";
    profile.year = "1998";
    profile.sound_driver = kCompressedSequencePlayer;
    profile.controller = true;
    profile.board_inputs.switches = kSwitches;
    profile.board_inputs.system = kSystemInputs;

    profile.snapshot_address = kMainLoopWait;
    profile.snapshot_hit = kWaitAfterReset;
    profile.quiet_tries = kUntilQuiet;
    profile.boot_seconds = 5.0;
    profile.stop_threads = {kFirstBoardThread, kSecondBoardThread};

    CallDriverOptions driver;
    driver.address = kAleck64DriverAddress;
    driver.start_calls = {{.function = kAskForSong, .arguments = {{.track = true}}}};
    profile.driver = BuildCallDriver(driver);

    profile.loop_watches.push_back(
        {.address = kLoopEnd, .target_register = mips::kV0, .track_register = mips::kT3, .count_register = mips::kA0});

    for (uint32_t number = 0; number < kSongs; number++)
    {
        if (number == kSameAsSong15)
        {
            continue;
        }

        profile.tracks.push_back(NumberedTrack(number, NumberTitle("Song", number, TitleBase::kDecimal)));
    }

    return profile;
}

} // namespace usf::rip::games
