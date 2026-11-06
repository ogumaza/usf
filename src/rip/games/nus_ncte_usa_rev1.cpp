// SPDX-License-Identifier: MIT

// Chameleon Twist (USA, Rev 1): libultra's compressed sequence player under the game's music code.
//
// Addresses come from the game's code in the image and from runs in usf's core. The main segment is copied from
// cartridge offset 0x1000 to 0x80025C00.
//
// - The sequence file's header is read from cartridge offset 0x27E930 by the audio set-up at 0x80085680, which keeps
//   its pointer at 0x801F8FD8. It holds 29 sequences.
// - The song-select function at 0x8008B57C takes the sequence's index in a0. It rejects an index outside the file's
//   count, stops the sequence that plays, and leaves the index in the request word at 0x800FB200. The game's sound
//   test calls it with every index from 0 to the count less one, so every sequence is a song the game can play.
// - The sound update at 0x8008B878 takes no arguments. The game thread calls it once a frame, every second video
//   field. Through 0x8008AC40 it starts a requested song once the player has stopped, at 0x8008B46C with the index
//   in v1.
// - The update restarts the current song once it has stopped only while the halfword at 0x801F84C4 is set. The table
//   of song settings at 0x800FB0B0 clears it for every song, and no other code sets it. A song without a loop
//   therefore plays once.
// - The sequence player's event reader at 0x800DAD34 handles a loop's end at 0x800DAEA8. It stores the jump back to
//   the loop in the track's read pointer at 0x800DAF1C, with the track in t3 and the new pointer in t9. 20 of the 29
//   sequences hold loops that repeat for ever. Sequence 0x1A repeats its loops once. The other eight hold none.
// - The game halts at a message that asks for a controller in the first socket when none is connected.

#include <cstdio>

#include "rip/driver.h"
#include "rip/games/games.h"

namespace usf::rip::games
{

namespace
{

constexpr uint32_t kSongSelect = 0x8008B57C;
constexpr uint32_t kSoundUpdate = 0x8008B878;
constexpr uint32_t kLoopJump = 0x800DAF1C;

// The last lines of the frame buffer that VI_ORIGIN points at (0x803DA800 to 0x80400000). Only the RDP writes there,
// and it draws nothing after the snapshot.
constexpr uint32_t kDriverAddress = 0x803FF000;

constexpr int kSequences = 29;

const char* TrackName(int index)
{
    switch (index)
    {
    case 0x19:
        return "Sequence 0x19: title screen";
    default:
        return nullptr;
    }
}

} // namespace

GameProfile NusNcteUsaRev1()
{
    GameProfile profile;
    profile.game = "Chameleon Twist";
    profile.serial = "NUS-NCTE-USA-rev1";
    profile.sha1 = "4ca6d563131b4809fe1748335182816a024999d4";
    profile.year = "1997";
    profile.cic = n64::Cic::k6102;
    profile.sound_driver = "libultra compressed sequence player";
    profile.controller = true;

    // The 64th call of the sound update comes on the screens before the title. No song or effect plays then. The driver
    // takes over the game thread there. It requests the track's song once, then runs the sound update every second
    // field, as the game does.
    profile.snapshot_address = kSoundUpdate;
    profile.snapshot_hit = 64;
    profile.quiet_tries = 30;
    profile.boot_seconds = 10.0;

    CallDriverOptions driver;
    driver.address = kDriverAddress;
    driver.start_calls = {{.function = kSongSelect, .arguments = {{.track = true}}}};
    driver.frame_calls = {{.function = kSoundUpdate}};
    driver.fields = 2;
    profile.driver = BuildCallDriver(driver);

    profile.loop_watches.push_back({.address = kLoopJump, .target_register = 25, .track_register = 11});

    for (int index = 0; index < kSequences; index++)
    {
        Track track;
        track.number = static_cast<uint32_t>(index);
        const char* name = TrackName(index);
        char fallback[32];
        std::snprintf(fallback, sizeof(fallback), "Sequence 0x%02X", index);
        track.name = name != nullptr ? name : fallback;
        profile.tracks.push_back(track);
    }

    return profile;
}

} // namespace usf::rip::games
