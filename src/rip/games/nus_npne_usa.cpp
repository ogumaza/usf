// SPDX-License-Identifier: MIT

// Pokémon Puzzle League (USA): the game's music, through Software Creations' music library. The game shows no titles
// for its songs. They go by their numbers in the game's song table.
//
// Addresses come from the game's code in the image and from runs in usf's core.
//
// - The game thread calls the function at 0x800039C0 in its main loop. That function also loads and starts the song
//   that a call of the function at 0x80005130 asked for. Those two functions take the song's level, pan and tempo from
//   the table at 0x800B3FB0.
// - The game's first call of the function at 0x800039C0 comes while the system is quiet. The snapshot takes over the
//   game thread there.
// - Each channel of a song loops separately. At 0x8008EAD4 the library has reached the end of a loop that repeats for
//   ever, with the channel in a0 and the loop's depth in a2.

#include <algorithm>
#include <cstdio>
#include <iterator>

#include "rip/driver.h"
#include "rip/games/games.h"
#include "rip/games/tracks.h"
#include "rip/mips.h"

namespace usf::rip::games
{
namespace
{

constexpr uint32_t kAudioUpdate = 0x800039C0;
constexpr uint32_t kPlaySong = 0x80005130;
constexpr uint32_t kSongTable = 0x800B3FB0;
constexpr uint32_t kEndlessLoop = 0x8008EAD4;

constexpr int kSongs = 72;

// The channels of a song loop at different periods. On one song the rip's length model ran to its limit. Each entry is
// the time that the slowest channel takes to reach its loop's second pass, measured in usf's core.
constexpr double kSongSeconds[] = {
    127.0, 121.0, 45.8,  30.8,  119.8, 83.4,  118.6, 158.0, 120.0, 179.8, 100.6, 73.0,  120.6, 179.8, 174.6,
    137.2, 73.2,  65.3,  169.2, 125.0, 71.2,  56.0,  109.8, 171.3, 90.0,  53.4,  83.4,  73.8,  120.0, 141.8,
    45.8,  30.8,  118.6, 52.0,  72.7,  140.1, 121.0, 182.2, 120.0, 179.3, 119.8, 179.2, 79.2,  71.5,  79.1,
    71.2,  71.2,  112.2, 109.8, 171.3, 90.0,  53.4,  83.4,  73.8,  119.8, 83.4,  90.0,  53.4,  119.8, 83.4,
    79.6,  127.0, 79.5,  120.2, 169.2, 193.0, 104.0, 179.0, 105.0, 26.5,  81.7,  211.2,
};

static_assert(std::ssize(kSongSeconds) == kSongs);

// These entries of the song table repeat an earlier entry: the same data and the same level, pan and tempo.
constexpr int kRepeats[] = {30, 31, 48, 49, 50, 51, 52, 53, 54, 55, 56, 58, 59, 61, 64};

// The end of RDRAM. A run of the game holds zeros there.
constexpr uint32_t kDriverAddress = 0x803FF000;

} // namespace

GameProfile NusNpneUsa()
{
    GameProfile profile;
    profile.game = "Pokémon Puzzle League";
    profile.serial = "NUS-NPNE-USA";
    profile.sha1 = "8173866fc8c7652abd44c48efcab85441c6806a1";
    profile.year = "2000";
    profile.cic = n64::Cic::k6102;
    profile.sound_driver = kSoftwareCreationsLibrary;
    profile.controller = true;

    profile.snapshot_address = kAudioUpdate;
    profile.snapshot_hit = 1;
    profile.quiet_tries = 1;
    profile.boot_seconds = 3.0;

    CallDriverOptions driver;
    driver.address = kDriverAddress;
    // a2 picks the song's player. The call passes 0 in a2 and a3. So does the game's call at 0x800065C4.
    driver.start_calls = {
        {.function = kPlaySong, .arguments = {{.value = kSongTable}, {.track = true}, {.value = 0}, {.value = 0}}}};
    driver.frame_calls = {{.function = kAudioUpdate}};
    profile.driver = BuildCallDriver(driver);

    profile.loop_watches.push_back(
        {.address = kEndlessLoop, .target_register = mips::kA2, .track_register = mips::kA0});

    for (int number = 0; number < kSongs; number++)
    {
        if (std::find(std::begin(kRepeats), std::end(kRepeats), number) != std::end(kRepeats))
        {
            continue;
        }

        Track track;
        track.number = static_cast<uint32_t>(number);
        char name[32];
        std::snprintf(name, sizeof(name), "Song %02d", number);
        track.name = name;
        track.length_seconds = kSongSeconds[number];
        track.fade_seconds = kMeasuredFadeSeconds;
        profile.tracks.push_back(track);
    }

    return profile;
}

} // namespace usf::rip::games
