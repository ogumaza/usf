// SPDX-License-Identifier: MIT

// Pokémon Snap (USA): the game's music. The game has no sound test. Each title names the course or the screen where the
// game plays the song.
//
// Addresses come from the game's code in the image and from runs in usf's core.
//
// - The audio thread (entry 0x800216A8) runs libultra's compressed sequence player, twice. The song table holds 37
//   songs. The game's main songs play on player 0. Player 1 plays ambience over them, such as a cave's or a river's.
// - The function at 0x80022374 takes a player in a0 and a song in a1, and sets the request that the audio thread reads.
//   The game thread makes the first call on the title screen. The snapshot takes over the game thread there.
// - The game sets its only reverb, the custom effect 6, through 0x80022300 before that call. The snapshot holds it.
// - The compressed sequence reader at 0x80028DA4 stores a loop's jump at 0x80028F34. The new read pointer is in t5, the
//   track's slot in a0 and the loop's count in a1.

#include "rip/driver.h"
#include "rip/games/games.h"
#include "rip/games/tracks.h"
#include "rip/mips.h"

namespace usf::rip::games
{
namespace
{

constexpr uint32_t kPlaySong = 0x80022374;
constexpr uint32_t kLoopJump = 0x80028F34;

// The last lines of a frame buffer at the end of RAM. Runs of the game hold zeros there. Only the RDP writes there, and
// it draws nothing after the snapshot.
constexpr uint32_t kDriverAddress = 0x803FF000;

constexpr uint32_t kSongs = 37;

// The code of the Cave course selects this song. It holds no notes that sound.
constexpr uint32_t kSilentSong = 0x16;

// Each track of a song loops separately, often at another period. The length model follows the first track to loop, and
// nested loops keep it from counting two passes of some tracks. Runs of the songs measured where each track's second
// pass ends. The table holds the latest of those ends.
constexpr SongLength kSongLengths[] = {
    {0x00, 188.8}, {0x04, 50.9},  {0x05, 155.0}, {0x06, 174.4}, {0x07, 145.1}, {0x08, 85.8}, {0x09, 93.0},
    {0x0A, 87.3},  {0x0C, 81.3},  {0x0D, 164.1}, {0x0E, 137.8}, {0x0F, 116.1}, {0x10, 79.9}, {0x12, 33.2},
    {0x13, 116.2}, {0x14, 64.0},  {0x15, 81.4},  {0x17, 48.2},  {0x18, 50.8},  {0x1A, 60.1}, {0x1B, 115.0},
    {0x1C, 19.1},  {0x1D, 142.3}, {0x1E, 116.1}, {0x22, 132.0},
};

// The titles of the tracks that have a name.
constexpr NamedNumber kTitles[] = {
    {0x00, "Song 0x00: Beach"},
    {0x01, "Song 0x01: Poké Flute, first song"},
    {0x02, "Song 0x02: Poké Flute, third song"},
    {0x03, "Song 0x03: Poké Flute, second song"},
    {0x04, "Song 0x04: Tunnel"},
    {0x05, "Song 0x05: River"},
    {0x06, "Song 0x06: Volcano"},
    {0x07, "Song 0x07: Valley"},
    {0x0B, "Song 0x0B: staff roll"},
    {0x0C, "Song 0x0C: new game"},
    {0x0D, "Song 0x0D: Cave"},
    {0x0E, "Song 0x0E: Rainbow Cloud"},
    {0x0F, "Song 0x0F: Cave, ambience"},
    {0x10, "Song 0x10: photo check"},
    {0x11, "Song 0x11: course finished"},
    {0x12, "Song 0x12: Professor Oak's report, part 1"},
    {0x14, "Song 0x14: River and title screen, ambience"},
    {0x15, "Song 0x15: River, ambience"},
    {0x17, "Song 0x17: main menu"},
    {0x18, "Song 0x18: Cave, second ambience"},
    {0x19, "Song 0x19: title screen"},
    {0x1B, "Song 0x1B: menu"},
    {0x1C, "Song 0x1C: Mew"},
    {0x1D, "Song 0x1D: Professor Oak's report, part 2"},
    {0x1E, "Song 0x1E: Professor Oak's report, part 3"},
    {0x1F, "Song 0x1F: photo check, ambience"},
    {0x20, "Song 0x20: Valley and Professor Oak's lab, ambience"},
    {0x21, "Song 0x21: Tunnel, ambience"},
    {0x22, "Song 0x22: gallery"},
    {0x23, "Song 0x23: main menu, ambience"},
    {0x24, "Song 0x24: staff roll, second part"},
};

} // namespace

GameProfile NusNpfeUsa()
{
    GameProfile profile;
    profile.game = "Pokémon Snap";
    profile.serial = "NUS-NPFE-USA";
    profile.sha1 = "edc7c49cc568c045fe48be0d18011c30f393cbaf";
    profile.year = "1999";
    profile.cic = n64::Cic::k6103;
    profile.sound_driver = kCompressedSequencePlayer;
    profile.controller = true;

    profile.snapshot_address = kPlaySong;
    profile.snapshot_hit = 1;
    profile.quiet_tries = 2;
    profile.boot_seconds = 5.0;

    CallDriverOptions driver;
    driver.address = kDriverAddress;
    driver.start_calls = {{.function = kPlaySong, .arguments = {{.value = 0}, {.track = true}}}};
    profile.driver = BuildCallDriver(driver);

    // The players' tracks jump back separately. The slot's address in a0 tells them apart.
    profile.loop_watches.push_back(
        {.address = kLoopJump, .target_register = mips::kT5, .track_register = mips::kA0, .count_register = mips::kA1});

    for (uint32_t index = 0; index < kSongs; index++)
    {
        if (index == kSilentSong)
        {
            continue;
        }

        profile.tracks.push_back(NamedOrNumberedTrack(index, kTitles, "Song", TitleBase::kHex, kSongLengths));
    }

    return profile;
}

} // namespace usf::rip::games
