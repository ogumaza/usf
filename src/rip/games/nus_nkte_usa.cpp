// SPDX-License-Identifier: MIT

// Mario Kart 64 (USA): the game's music, played through the sound engine that Super Mario 64 also uses. This is a
// later revision of it, with a queue of commands from the game thread to the audio thread.
//
// Addresses come from the game's code in the image and from runs in usf's core. The main segment is copied from
// cartridge offset 0x1000 to 0x80000400.
//
// - The sequence table at cartridge offset 0xBC5F60 holds 30 sequences. The audio set-up at 0x800C5C40 starts
//   sequence 0 on sequence player 2 (through 0x800C284C, at 0x800C5C94), for the sound effects. The other 29 are the
//   tracks.
// - The play-sequence function at 0x800C8EAC takes a sequence's number in a0. It queues command 0x00010000 with the
//   number through 0x800C3448, which starts the sequence on sequence player 0.
// - The game-loop tick at 0x800CB2C4 takes no arguments. It sends the queued commands to the audio thread through
//   0x800CBC24. The game thread (0x801589D0, created at 0x800024F4) calls it from its loop at 0x8000277C, once a
//   frame, every second video field.
// - The tick's first call comes before any sound. The game then resets the audio and first plays sequence 1 on the
//   title screen.
// - The sequence player's script reader handles its jumps (commands 0xF5 and 0xF9 to 0xFB) at 0x800C0C54 to
//   0x800C0CA8. At 0x800C0CA4 the new script address is in t3 and the sequence player in s1.
// - The reader handles the end of a counted loop (command 0xF7) at 0x800C0C0C and the relative jumps (commands 0xF2
//   to 0xF4) at 0x800C0CAC. The watch below doesn't see them.

#include <cstdio>

#include "rip/driver.h"
#include "rip/games/games.h"

namespace usf::rip::games
{

namespace
{

constexpr uint32_t kPlaySequence = 0x800C8EAC;
constexpr uint32_t kGameLoopTick = 0x800CB2C4;
constexpr uint32_t kSequenceJump = 0x800C0CA4;

// The last lines of the frame buffer at 0x803DA800 to 0x80400000. Runs of the game hold zeros there. Only the RDP
// writes there, and it draws nothing after the snapshot.
constexpr uint32_t kDriverAddress = 0x803FF000;

constexpr int kSequences = 30;

// The length analysis follows the part that loops last. Sequence player 2 runs the sound effects' script, which jumps
// back every 207.7 s. That gave 0x10 a length of 415.5 s and 0x12 the limit of 600 s. Runs with watches on every
// loop command of the scripts measured where the music's second pass ends:
// - 0x10 loops through one of the commands that the watch doesn't see, every 49.36 s from 49.43 s. Its music
//   alternates two passes, so it repeats every 98.72 s, from the start.
// - 0x12 jumps back every 131.80 s from 131.85 s.
struct TrackLength
{
    int number;
    double seconds;
};

constexpr TrackLength kTrackLengths[] = {{0x10, 197.5}, {0x12, 263.6}};

constexpr double kFadeSeconds = 10.0;

const char* TrackName(int index)
{
    switch (index)
    {
    case 0x01:
        return "Sequence 0x01: title screen";
    default:
        return nullptr;
    }
}

} // namespace

GameProfile NusNkteUsa()
{
    GameProfile profile;
    profile.game = "Mario Kart 64";
    profile.serial = "NUS-NKTE-USA";
    profile.sha1 = "579c48e211ae952530ffc8738709f078d5dd215e";
    profile.year = "1997";
    profile.cic = n64::Cic::k6102;
    profile.sound_driver = "Super Mario 64 sound engine";
    profile.controller = true;
    profile.eeprom = n64::EepromType::k4Kbit;

    // The driver takes over the game thread at the tick's first call. It starts the track's sequence once, then runs
    // the tick every second field, as the game does. Later calls of the tick aren't quiet: the audio thread keeps the
    // AI's second buffer waiting, and the RSP runs the game's graphics.
    profile.snapshot_address = kGameLoopTick;
    profile.snapshot_hit = 1;
    profile.quiet_tries = 2;
    profile.boot_seconds = 5.0;

    CallDriverOptions driver;
    driver.address = kDriverAddress;
    driver.start_calls = {{.function = kPlaySequence, .arguments = {{.track = true}}}};
    driver.frame_calls = {{.function = kGameLoopTick}};
    driver.fields = 2;
    profile.driver = BuildCallDriver(driver);

    // The sequence players run separate scripts. The player's address in s1 tells their jumps apart.
    profile.loop_watches.push_back({.address = kSequenceJump, .target_register = 11, .track_register = 17});

    for (int index = 1; index < kSequences; index++)
    {
        Track track;
        track.number = static_cast<uint32_t>(index);
        const char* name = TrackName(index);
        char fallback[32];
        std::snprintf(fallback, sizeof(fallback), "Sequence 0x%02X", index);
        track.name = name != nullptr ? name : fallback;
        for (const TrackLength& length : kTrackLengths)
        {
            if (length.number == index)
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
