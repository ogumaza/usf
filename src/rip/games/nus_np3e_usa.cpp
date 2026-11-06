// SPDX-License-Identifier: MIT

// Pokémon Stadium 2 (USA): the game's music, through the sound engine of Nintendo's Zelda games. The game has no sound
// test. Sequences go by the numbers in the game's sequence table.
//
// Addresses come from the game's code in the image and from runs in usf's core.
//
// - The function at 0x80021ED8 is the game's call for music. It starts sequence number + 0x27 on sequence player 0,
//   scales the player's volume by a level for each song, and sets a change of tempo and masks of channels for some
//   songs. The profile calls it for that reason. The function at 0x800187AC then sends the commands to the audio
//   thread.
// - The game resets its audio engine to a set-up for each mode, in the function at 0x800260C0. Boot's set-up runs at
//   28.8 kHz. The title's set-up, 1, runs at 32 kHz, like those of the menus and most other modes. Battles use set-ups
//   2 to 4 at 28.8 kHz. All of them hold the same first reverb, the one that the sequences use.
// - The snapshot takes over the game thread at the first call of the music function, for the title's song. The reset to
//   set-up 1 has ended by then. An earlier snapshot would change the rate of the output during the song.
// - One function runs the jumps of all scripts. At 0x8001C3A8 the new script address is in t9 and the script's state in
//   a1. The state of sequence player 0's script is at 0x8011F374.

#include <array>
#include <cstdio>

#include "rip/driver.h"
#include "rip/games/games.h"

namespace usf::rip::games
{

namespace
{

constexpr uint32_t kPlayMusic = 0x80021ED8;
constexpr uint32_t kSendCommands = 0x800187AC;
constexpr uint32_t kSequenceJump = 0x8001C3A8;
constexpr uint32_t kSequenceScriptState = 0x8011F374;
constexpr uint32_t kResetAudio = 0x80018B78;
constexpr uint32_t kReceiveMessage = 0x80073CE0;
constexpr uint32_t kResetDoneQueue = 0x801221F0;
constexpr uint32_t kBlock = 1;
constexpr uint32_t kAllMask = 0xFFFFFFFF;

// Songs 0x51 and 0x52 play sequences 0x78 and 0x79. They hold 35,200 and 20,032 bytes. Only audio set-up 8 has room for
// them, with a sequence cache of 37,376 bytes. The others have 14,336. The game's mode 13 uses that set-up for them.
// The function at 0x80018B78 resets the audio engine to the set-up in a0. The driver calls it for these songs and then
// waits until a message on the queue at 0x801221F0 marks the end of the reset.
constexpr uint32_t kLargeSetup = 8;
constexpr std::array<uint32_t, 2> kLargeSongs = {0x51, 0x52};

// The last lines of a frame buffer at the end of RAM. Only the RDP writes there, and it draws nothing after the
// snapshot.
constexpr uint32_t kDriverAddress = 0x803FF000;

// The game's music function takes a number and plays the sequence 0x27 higher. Its debug menu has a music room with
// numbers 1 to 0x61. Sequence 0x81 repeats 0x28, and sequences 0x82 and 0x84 repeat 0x7B. The lower sequences make no
// sound alone.
constexpr int kFirstSequence = 0x27;
constexpr int kMusicNumbers = 0x62;

// The function also takes these numbers, for versions of five songs. The first three of them run faster through a
// change of tempo. The other two mute different channels: they are the versions that the game switches to in play.
struct Version
{
    uint32_t number;
    const char* name;
};

constexpr std::array kVersions = {
    Version{0x64, "Sequence 0x49, faster"},         Version{0x65, "Sequence 0x5D, faster"},
    Version{0x68, "Sequence 0x73, faster"},         Version{0x66, "Sequence 0x65, second version"},
    Version{0x67, "Sequence 0x2E, second version"},
};

constexpr bool IsMusic(int sequence)
{
    return sequence != 0x81 && sequence != 0x82 && sequence != 0x84;
}

} // namespace

GameProfile NusNp3eUsa()
{
    GameProfile profile;
    profile.game = "Pokémon Stadium 2";
    profile.serial = "NUS-NP3E-USA";
    profile.sha1 = "d8343e69a7dc63b869cf6361d87cde64444281d3";
    profile.year = "2001";
    profile.cic = n64::Cic::k6103;
    profile.sound_driver = "Zelda sound engine";
    profile.controller = true;

    profile.snapshot_address = kPlayMusic;
    profile.snapshot_hit = 1;
    profile.quiet_tries = 1;
    profile.boot_seconds = 26.0;

    CallDriverOptions driver;
    driver.address = kDriverAddress;
    for (const uint32_t number : kLargeSongs)
    {
        driver.start_calls.push_back({.function = kResetAudio,
                                      .arguments = {{.value = kLargeSetup}},
                                      .track_mask = kAllMask,
                                      .track_value = number});
        driver.start_calls.push_back({.function = kReceiveMessage,
                                      .arguments = {{.value = kResetDoneQueue}, {.value = 0}, {.value = kBlock}},
                                      .track_mask = kAllMask,
                                      .track_value = number});
    }

    driver.start_calls.push_back({.function = kPlayMusic, .arguments = {{.track = true}}});
    driver.start_calls.push_back({.function = kSendCommands});
    profile.driver = BuildCallDriver(driver);

    profile.loop_watches.push_back(
        {.address = kSequenceJump, .target_register = 25, .match_register = 5, .match_value = kSequenceScriptState});

    for (int number = 1; number < kMusicNumbers; number++)
    {
        const int sequence = number + kFirstSequence;
        if (!IsMusic(sequence))
        {
            continue;
        }

        Track track;
        track.number = static_cast<uint32_t>(number);
        char name[32];
        std::snprintf(name, sizeof(name), "Sequence 0x%02X", sequence);
        track.name = name;
        profile.tracks.push_back(track);
    }

    for (const Version& version : kVersions)
    {
        Track track;
        track.number = version.number;
        track.name = version.name;
        profile.tracks.push_back(track);
    }

    return profile;
}

} // namespace usf::rip::games
