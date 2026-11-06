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

#include <array>
#include <cstdio>

#include "rip/driver.h"
#include "rip/games/games.h"

namespace usf::rip::games
{

namespace
{

constexpr uint32_t kPlayMusic = 0x70006E7C;
constexpr uint32_t kLoopJump = 0x7001268C;

// RDRAM from 0x803C0000 stays clear for as long as the game runs.
constexpr uint32_t kDriverAddress = 0x803FF000;

constexpr std::array<const char*, 63> kTitles = {
    nullptr,
    "short death",
    "intro",
    "Train",
    "Depot",
    "multiplayer 1",
    "Citadel",
    "Facility",
    "Control",
    "Dam",
    "Frigate",
    "Archives",
    "Silo",
    "multiplayer 2",
    "Streets",
    "Bunker I",
    "Bunker II",
    "Statue",
    "Control, elevator",
    "Cradle",
    nullptr,
    "Caverns, elevator",
    "Egyptian",
    "folders",
    "watch",
    "Aztec",
    "Caverns",
    "death",
    "Surface II",
    "Train, second track",
    nullptr,
    "Facility, second track",
    "Depot, second track",
    "Control, second track",
    "Caverns, second track",
    "Dam, second track",
    "Frigate, second track 1",
    "Archives, second track",
    "Silo, second track",
    nullptr,
    "Streets, second track",
    "Bunker I, second track",
    "Bunker II, second track",
    "Jungle, second track",
    "first sound of the intro",
    "Statue, second track 1",
    "Aztec, second track",
    "Egyptian, second track",
    "Cradle, second track",
    "Cuba",
    "Runway",
    "Runway, second track 2",
    "multiplayer 3",
    "wind",
    "guitar gliss",
    "Jungle",
    "Runway, second track 1",
    "Surface I",
    "multiplayer death",
    "Surface II, second track 1",
    "Surface II, second track 2",
    "Statue, second track 2",
    "Frigate, second track 2",
};

// A sequence's tracks loop separately, and some of them repeat a short figure for ever. The length analysis counts the
// first track that loops, which ends these songs too soon. Runs that printed every track's jumps give their lengths:
// the longest track's first jump and one more pass.
struct SongLength
{
    uint32_t index;
    double seconds;
};

constexpr SongLength kSongLengths[] = {
    {.index = 0x08, .seconds = 264.0}, {.index = 0x0B, .seconds = 270.3}, {.index = 0x0D, .seconds = 265.5},
    {.index = 0x18, .seconds = 72.0},  {.index = 0x1A, .seconds = 499.6}, {.index = 0x1C, .seconds = 307.2},
    {.index = 0x22, .seconds = 109.8}, {.index = 0x29, .seconds = 122.9}, {.index = 0x2A, .seconds = 109.8},
    {.index = 0x2B, .seconds = 93.6},  {.index = 0x2C, .seconds = 94.2},  {.index = 0x2E, .seconds = 112.0},
    {.index = 0x2F, .seconds = 111.3}, {.index = 0x32, .seconds = 299.6}, {.index = 0x37, .seconds = 476.4},
};

constexpr double kFadeSeconds = 10.0;

} // namespace

GameProfile NusNgeeUsa()
{
    GameProfile profile;
    profile.game = "GoldenEye 007";
    profile.serial = "NUS-NGEE-USA";
    profile.sha1 = "abe01e4aeb033b6c0836819f549c791b26cfde83";
    profile.year = "1997";
    profile.cic = n64::Cic::k6102;
    profile.sound_driver = "libultra compressed sequence player";
    profile.controller = true;
    profile.eeprom = n64::EepromType::k4Kbit;

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
        {.address = kLoopJump, .target_register = 25, .track_register = 11, .count_register = 4});

    // Entries 0x14, 0x1E and 0x27 play nothing. The set leaves them out.
    for (uint32_t index = 1; index < kTitles.size(); index++)
    {
        if (kTitles[index] == nullptr)
        {
            continue;
        }

        Track track;
        track.number = index;
        char name[64];
        std::snprintf(name, sizeof(name), "Sequence 0x%02X: %s", index, kTitles[index]);
        track.name = name;
        for (const SongLength& length : kSongLengths)
        {
            if (length.index == index)
            {
                track.length_seconds = length.seconds;
                track.fade_seconds = kFadeSeconds;
            }
        }
        profile.tracks.push_back(track);
    }

    return profile;
}

} // namespace usf::rip::games
