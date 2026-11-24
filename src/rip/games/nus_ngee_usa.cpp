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
// - The table at 0x8004EB10 has 8 bytes for each stage: its number, its main track, a track of background sound and its
//   second track. A run with the controller requested 0x2C and 0x02 in the intro, 0x17 after the title, and 0x09 in the
//   first stage: Dam.
// - The function at 0x7F0C11FC starts the music of a stage. The stage load calls it with the stage number. The function
//   stops the three players and reads the entry of the stage in the table. The main track plays on the first player at
//   the music volume. Surface 2 (stage 0x2B) is the only stage with a background track. The background 0x35 plays on
//   the third player at the volume of the sound effects, together with the main track 0x1C. The code above 0x7F000000
//   is paged in through the TLB. The call also runs the two volume functions at 0x7F0A9180 and 0x7F0A9358. Their page
//   is not in memory at the snapshot, and the TLB handlers of the game load it from the cartridge during the call.
// - The list of random music for multiplayer, at 0x8004EBD0, plays 0x1C alone. The set keeps that track too, after the
//   other tracks.
// - The sequence player's event reader stores the jump back to a loop at 0x7001268C, with the track in t3, the new
//   pointer in t9 and the loop's repeat count in a0. The songs repeat short figures with counts below 0xFF. Only a
//   count of 0xFF shows a song's loop.

#include <algorithm>
#include <cstdint>
#include <iterator>

#include "rip/driver.h"
#include "rip/games/games.h"
#include "rip/games/tracks.h"
#include "rip/mips.h"

namespace usf::rip::games
{
namespace
{

constexpr uint32_t kPlayMusic = 0x70006E7C;
constexpr uint32_t kStartStageMusic = 0x7F0C11FC;
constexpr uint32_t kLoopJump = 0x7001268C;

// The stage number of Surface 2, the main track and the background track. The number of the stage music track
// contains both sequence numbers, with the background in the second byte.
constexpr uint32_t kSurface2Stage = 0x2B;
constexpr uint32_t kSurface2Main = 0x1C;
constexpr uint32_t kSurface2Background = 0x35;
constexpr uint32_t kBackgroundMask = 0xFF00;
constexpr uint32_t kBackgroundShift = 8;
constexpr uint32_t kSurface2Track = kSurface2Main | (kSurface2Background << kBackgroundShift);

// The entries of the table of sequences. Entry 0 and the entries of kSilent play nothing.
constexpr uint32_t kSequences = 0x3F;
constexpr uint32_t kSilent[] = {0x14, 0x1E, 0x27};

// RDRAM from 0x803C0000 stays clear for as long as the game runs.
constexpr uint32_t kDriverAddress = 0x803FF000;

// A sequence's tracks loop separately, and some of them repeat a short figure for ever. The length analysis counts the
// first track that loops. That ends these songs too soon. Runs that printed every track's jumps give their lengths: the
// longest track's first jump and one more pass.
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
    driver.start_calls = {
        {.function = kPlayMusic, .arguments = {{.track = true}}, .track_mask = kBackgroundMask, .track_value = 0},
        {.function = kStartStageMusic,
         .arguments = {{.value = kSurface2Stage}},
         .track_mask = kBackgroundMask,
         .track_value = kSurface2Background << kBackgroundShift}};
    profile.driver = BuildCallDriver(driver);

    profile.loop_watches.push_back(
        {.address = kLoopJump, .target_register = mips::kT9, .track_register = mips::kT3, .count_register = mips::kA0});

    // The set makes a track for each entry of the table, apart from kSilent. In the place of 0x1C, the track is the
    // music of Surface 2 as the stage start plays it: 0x1C with the background 0x35. The track uses the length of 0x1C.
    // The background loops for ever, and the loop watch would count those loops too.
    for (uint32_t number = 1; number < kSequences; number++)
    {
        if (std::ranges::find(kSilent, number) != std::end(kSilent))
        {
            continue;
        }

        Track track = NumberedTrack(number, NumberTitle("Sequence", number, TitleBase::kHex), kSongLengths);
        if (number == kSurface2Main)
        {
            track.number = kSurface2Track;
        }
        profile.tracks.push_back(track);
    }

    // 0x1C alone, as the multiplayer list plays it, follows the other tracks.
    profile.tracks.push_back(
        NumberedTrack(kSurface2Main, NumberTitle("Sequence", kSurface2Main, TitleBase::kHex), kSongLengths));

    return profile;
}

} // namespace usf::rip::games
