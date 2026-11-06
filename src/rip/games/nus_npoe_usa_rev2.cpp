// SPDX-License-Identifier: MIT

// Pokémon Stadium (USA, revision 2): the game's music, through Software Creations' music library. The game has no sound
// test. Songs go by the game's numbers.
//
// Addresses come from the game's code in the image and from runs in usf's core.
//
// - The function at 0x8004B30C takes a song's number in a0. It reads the song's file from the cartridge through a
//   table, unpacks it, starts it with the sample bank that boot loads, and sets the song's volume from a switch on the
//   number. The game's first call comes at a point that isn't quiet.
// - The game thread's function at 0x80029028 loads the next part of the game's code. Its first call comes after the
//   sample bank's load. That point is quiet. Later points in the game thread come right after a retrace, when the audio
//   thread keeps AI's second buffer waiting. The snapshot takes over the game thread at that first call.
// - Each channel of a song loops separately, through the loop-end command at 0x80038710. At 0x80038724 the channel is
//   in a0, the loop's level in v0 and its count in a2. A count of 0xFF repeats for ever.

#include <cstdio>

#include "rip/driver.h"
#include "rip/games/games.h"

namespace usf::rip::games
{

namespace
{

constexpr uint32_t kFirstState = 0x80029028;
constexpr uint32_t kPlaySong = 0x8004B30C;
constexpr uint32_t kLoopEnd = 0x80038724;

// The last lines of a frame buffer at the end of RAM. Only the RDP writes there, and it draws nothing after the
// snapshot.
constexpr uint32_t kDriverAddress = 0x803FF000;

// The numbers that the function at 0x8004B30C accepts. Number 0x50 only fades number 0x28. Numbers 0x5A and 0x5B play
// numbers 0x02 and 0x01 at other levels. The set leaves those three out.
constexpr int kSongs[] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12,
    0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25,
    0x26, 0x27, 0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38,
    0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4A, 0x4B};

// Each channel of a song loops separately, often at another period. Runs of the songs measured where each channel's
// second pass ends. The table holds the latest of those ends.
struct SongLength
{
    int number;
    double seconds;
};

constexpr SongLength kSongLengths[] = {
    {0x01, 175.1}, {0x02, 111.0}, {0x03, 265.5}, {0x04, 146.2}, {0x05, 196.7}, {0x06, 268.4}, {0x07, 58.1},
    {0x08, 73.7},  {0x09, 82.0},  {0x0A, 22.0},  {0x0B, 199.1}, {0x0C, 137.4}, {0x0D, 203.4}, {0x0E, 230.8},
    {0x0F, 134.9}, {0x10, 64.2},  {0x11, 41.2},  {0x12, 275.4}, {0x14, 361.9}, {0x15, 82.3},  {0x16, 106.3},
    {0x17, 23.5},  {0x18, 65.0},  {0x19, 79.5},  {0x1C, 43.0},  {0x1D, 57.4},  {0x1E, 23.2},  {0x21, 101.0},
    {0x22, 257.6}, {0x23, 189.4}, {0x24, 149.9}, {0x25, 172.7}, {0x26, 204.9}, {0x27, 85.9},  {0x28, 69.1},
    {0x2A, 164.5}, {0x2B, 127.7}, {0x2D, 98.3},  {0x2E, 205.2}, {0x2F, 247.2}, {0x30, 130.0}, {0x32, 46.4},
    {0x34, 188.2}, {0x35, 256.8}, {0x36, 193.4}, {0x37, 150.0}, {0x39, 239.8}, {0x3B, 202.1}, {0x45, 50.5},
    {0x47, 35.5},  {0x49, 43.8},  {0x4B, 97.8},
};

constexpr double kFadeSeconds = 10.0;

} // namespace

GameProfile NusNpoeUsaRev2()
{
    GameProfile profile;
    profile.game = "Pokémon Stadium";
    profile.serial = "NUS-NPOE-USA-rev2";
    profile.sha1 = "0d3b1d740c6ee6da6923550bf24ad827262fd8c0";
    profile.year = "2000";
    profile.cic = n64::Cic::k6103;
    profile.sound_driver = "Software Creations' music library";
    profile.controller = true;

    profile.snapshot_address = kFirstState;
    profile.snapshot_hit = 1;
    profile.quiet_tries = 1;
    profile.boot_seconds = 3.0;

    CallDriverOptions driver;
    driver.address = kDriverAddress;
    driver.start_calls = {{.function = kPlaySong, .arguments = {{.track = true}}}};
    profile.driver = BuildCallDriver(driver);

    profile.loop_watches.push_back(
        {.address = kLoopEnd, .target_register = 2, .track_register = 4, .count_register = 6});

    for (const int number : kSongs)
    {
        Track track;
        track.number = static_cast<uint32_t>(number);
        char name[32];
        std::snprintf(name, sizeof(name), "Song 0x%02X", number);
        track.name = name;
        for (const SongLength& length : kSongLengths)
        {
            if (length.number == number)
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
