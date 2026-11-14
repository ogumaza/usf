// SPDX-License-Identifier: MIT

// GoldenEye 007 (USA): libultra's compressed sequence player under the game's music code.
//
// Addresses come from the game's code in the image and from runs in usf's core. The main segment is mapped at
// 0x70000400 from cartridge offset 0x1000.
//
// - The table of compressed sequences is at 0x80063734, 63 entries at the snapshot. Entry 0 is empty.
// - The function at 0x70006E7C plays a sequence on the first player: it stops what plays, reads the sequence from the
//   cartridge, unpacks it and starts the player, all before it returns. The game runs a second and a third player the
//   same way, for the stages' second tracks and for death.
// - The table at 0x8004EB10 holds 8 bytes for each stage: its number, its main track, a track of background sound and
//   its second track. The stages' titles come from it and from the stages' names, which the game shows. A run with the
//   controller asked for 0x2C and 0x02 in the intro, 0x17 after the title, and 0x09 in the first stage: Dam.
// - The sequence player's event reader stores the jump back to a loop at 0x7001268C, with the track in t3, the new
//   pointer in t9 and the loop's repeat count in a0. The songs repeat short figures with counts below 0xFF, so only a
//   count of 0xFF shows a song's loop.

#include <cstdint>

#include "rip/driver.h"
#include "rip/games/games.h"
#include "rip/games/tracks.h"
#include "rip/mips.h"

namespace usf::rip::games
{
namespace
{

constexpr uint32_t kPlayMusic = 0x70006E7C;
constexpr uint32_t kLoopJump = 0x7001268C;

// RDRAM from 0x803C0000 stays clear for as long as the game runs.
constexpr uint32_t kDriverAddress = 0x803FF000;

// A sequence's tracks loop separately, and some of them repeat a short figure for ever. The length analysis counts the
// first track that loops, which ends these songs too soon. Runs that printed every track's jumps give their lengths:
// the longest track's first jump and one more pass.
constexpr SongLength kSongLengths[] = {
    {.number = 0x08, .seconds = 264.0}, {.number = 0x0B, .seconds = 270.3}, {.number = 0x0D, .seconds = 265.5},
    {.number = 0x18, .seconds = 72.0},  {.number = 0x1A, .seconds = 499.6}, {.number = 0x1C, .seconds = 307.2},
    {.number = 0x22, .seconds = 109.8}, {.number = 0x29, .seconds = 122.9}, {.number = 0x2A, .seconds = 109.8},
    {.number = 0x2B, .seconds = 93.6},  {.number = 0x2C, .seconds = 94.2},  {.number = 0x2E, .seconds = 112.0},
    {.number = 0x2F, .seconds = 111.3}, {.number = 0x32, .seconds = 299.6}, {.number = 0x37, .seconds = 476.4},
};

} // namespace

GameProfile NusNgeeUsa()
{
    GameProfile profile;
    profile.game = "GoldenEye 007";
    profile.serial = "NUS-NGEE-USA";
    profile.sha1 = "abe01e4aeb033b6c0836819f549c791b26cfde83";
    profile.year = "1997";
    profile.cic = ultralib::machine::Cic::k6102;
    profile.sound_driver = kCompressedSequencePlayer;
    profile.controller = true;
    profile.eeprom = ultralib::machine::EepromType::k4Kbit;

    // The game's first request for music is the swoosh of the intro. Nothing plays yet. The driver takes over the game
    // thread there, plays the track and idles. The audio thread does the rest.
    profile.snapshot_address = kPlayMusic;
    profile.snapshot_hit = 1;
    profile.quiet_tries = 1;
    profile.boot_seconds = 10.0;

    CallDriverOptions driver;
    driver.address = kDriverAddress;
    driver.start_calls = {{.function = kPlayMusic, .arguments = {{.track = true}}}};
    profile.driver = BuildCallDriver(driver);

    profile.loop_watches.push_back(
        {.address = kLoopJump, .target_register = mips::kT9, .track_register = mips::kT3, .count_register = mips::kA0});

    // The set makes a track for each name in ultralib's table. Entries 0x00, 0x14, 0x1E and 0x27 have no name there and
    // play nothing. A new name in the table adds a track. Compare usfrip --list, then update the count.
    static_assert(ultralib::n64::kGoldenEyeSongNames.size() == 59);
    for (const SongName& song : ultralib::n64::kGoldenEyeSongNames)
    {
        const auto number = static_cast<uint32_t>(song.index);
        profile.tracks.push_back(
            NumberedTrack(number, SongTitle("Sequence", number, TitleBase::kHex, song.name), kSongLengths));
    }

    return profile;
}

} // namespace usf::rip::games
