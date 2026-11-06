// SPDX-License-Identifier: MIT

// Cruis'n World (USA): libultra's compressed sequence player under the game's sound commands.
//
// Addresses come from the game's code in the image and from runs in usf's core. The main segment is copied from
// cartridge offset 0x1000 to 0x802AD400.
//
// - The sequence file at cartridge offset 0x82CF10 holds 14 sequences. Its pointer is kept at 0x80395690. Songs 0 to
//   3 are short sequences of 116 to 151 bytes. Each plays long samples of the music bank.
// - The song-select function at 0x80300374 takes the song's number in a0. It posts command 8 to the sound command
//   queue at 0x803D1650. The audio thread runs the queue in each audio frame through 0x80300DB4. Command 8 stops the
//   player and leaves the number in the request word at 0x803956D4. After a delay of 30 audio frames, 0x802AF2D8
//   loads the sequence (0x802AF094) and plays it (0x802AF104).
// - The front end requests song 0 on the title screen and song 1 in the menus. It requests song 3 at five places in
//   the front-end overlay (0x8005F560, 0x80060730, 0x800607AC, 0x800671A8 and 0x80067234), in the screens that show
//   a race's total time and a championship's points. No code requests song 2 by a constant. A race requests its
//   course's song from the table of courses at 0x8034A450 (60 bytes for each course, the song at offset 0x10). The
//   courses use songs 4 to 13.
// - The course names at 0x803448C0 are New York, England, France, Germany, Italy, Egypt, Russia, Kenya, China, Japan,
//   Australia, Hawaii, Mexico, Florida and Moon. The menu's order table at 0x80344900 gives the number of the course
//   chosen, kept at 0x803BF2EF. The championship reads the entry of the table of courses from the halfwords at
//   0x8033B67C and adds 15 to the cruise's entry for the same course (0x8033B69C, numbered from 1). Runs of the
//   championship with each number asked for each song: one with the controller alone showed GERMANY, reached entry 15
//   and asked for song 0x0B. Entries n and n + 15 hold the same course's data. The only song that differs between them
//   is the Moon's: song 9 in a cruise and song 8 in a championship.
// - The threads: 0x8035B130 is the game thread (priority 10), 0x80393178 the audio thread (priority 30) and
//   0x8035E540 the scheduler (priority 40). osCreateThread is at 0x80319890.
// - The sequence player's event reader at 0x8031CDB4 stores the jump back to a loop in the track's read pointer at
//   0x8031CF9C, with the track in t3 and the new pointer in t9. Every sequence loops for ever.
// - The game shows a message that asks for a controller when none is connected, and plays no song then.
// - The race's effect table is at 0x80333990: 12 bytes for each effect. The function at 0x802AEB2C installs a table: it
//   leaves the table's address at 0x803956C0 and gives each effect a sound state of the player at 0x80395490. An
//   effect's first halfword picks its sound from the list at 0x803956E0. The game posts an effect through 0x80300590 as
//   command 0 of the queue.
// - When the player's car finishes, 0x802B2C34 reads its place at offset 0x388. The process at 0x802B2740 then plays
//   the finish's sounds: effect 5 in first place, effect 6 in second or third, and effect 7 in fourth or lower. The
//   first three places also play effect 0x29, a looped sound, and first place plays effect 0x28 too. Runs of a race in
//   which a patch made the car finish in each place played these effects. The code at 0x802AEEF8 stops every other
//   effect while these play. They're samples of the sound-effect bank (cartridge offset 0x5C4C60), so no song can play
//   them. Each one's track writes the sound state and the queue's first command. The songs play here without the race's
//   other sounds, and the fanfares play without effects 0x28 and 0x29 in the same way.

#include <cstdio>

#include "rip/games/games.h"

namespace usf::rip::games
{

namespace
{

// The audio frame at 0x802AE150 runs in the audio thread. At 0x802AE184 it calls 0x802AF7F4, which runs the sound
// commands. The frame then hands the AI its next buffer at 0x802AE1B8 and, at its end, updates the music through
// 0x802AF2D8.
constexpr uint32_t kAudioFrameCommands = 0x802AE184;

// The music update's words: a song waits to be loaded (0x803334B4), the audio frames left before it loads
// (0x803956D0) and the requested song (0x803956D4).
constexpr uint32_t kSongPending = 0x803334B4;
constexpr uint32_t kSongDelay = 0x803956D0;
constexpr uint32_t kSongRequest = 0x803956D4;

constexpr uint32_t kGameThread = 0x8035B130;
constexpr uint32_t kLoopJump = 0x8031CF9C;

constexpr int kSongs = 14;

// The race's effect table, and the fields of an effect there: its rate at offset 4.
constexpr uint32_t kEffectTablePointer = 0x803956C0;
constexpr uint32_t kRaceEffects = 0x80333990;
constexpr uint32_t kEffectSize = 12;
constexpr uint32_t kEffectRate = 0x56220000; // 22050 Hz, in the upper halfword.

// The sound player's first sound state. Every effect's handle is 0 at the snapshot. 0x8031C990 fills these words of a
// state when it allocates one: the sound, the priority, the pitch, the state and the volume and pan.
constexpr uint32_t kSoundState0 = 0x8037721C;

// The queue's first command: command 0, no handle, the effect, its volume, the centre pan, no fade, its sequence
// number and the word that marks the command as waiting.
constexpr uint32_t kSoundQueue = 0x803D1650;

struct Fanfare
{
    uint32_t effect;
    uint32_t sound;  // The ALSound in the list at 0x803956E0.
    uint32_t volume; // The effect's third byte in the race's table.
    double seconds;
    const char* name;
};

constexpr Fanfare kFanfares[] = {
    {.effect = 5, .sound = 0x8037AB88, .volume = 0xC8, .seconds = 8.2, .name = "Effect 0x05: first place fanfare"},
    {.effect = 6,
     .sound = 0x8037AC50,
     .volume = 0xAA,
     .seconds = 4.4,
     .name = "Effect 0x06: second or third place fanfare"},
    {.effect = 7, .sound = 0x8037AD18, .volume = 0xAF, .seconds = 2.7, .name = "Effect 0x07: fourth place or lower"},
};

const char* TrackName(int index)
{
    switch (index)
    {
    case 0x00:
        return "Song 0x00: title screen";
    case 0x01:
        return "Song 0x01: menus";
    case 0x03:
        return "Song 0x03: race results";
    case 0x04:
        return "Song 0x04: China, Japan";
    case 0x05:
        return "Song 0x05: Mexico";
    case 0x06:
        return "Song 0x06: Hawaii, Florida";
    case 0x07:
        return "Song 0x07: Italy";
    case 0x08:
        return "Song 0x08: New York, Russia, Australia, Moon (Championship)";
    case 0x09:
        return "Song 0x09: France, Moon (Cruise)";
    case 0x0A:
        return "Song 0x0A: England";
    case 0x0B:
        return "Song 0x0B: Germany";
    case 0x0C:
        return "Song 0x0C: Egypt";
    case 0x0D:
        return "Song 0x0D: Kenya";
    default:
        return nullptr;
    }
}

} // namespace

GameProfile NusNcweUsa()
{
    GameProfile profile;
    profile.game = "Cruis'n World";
    profile.serial = "NUS-NCWE-USA";
    profile.sha1 = "6da1a6a2bda687d50e798d80c342948ad1738202";
    profile.year = "1998";
    profile.cic = n64::Cic::k6106;
    profile.sound_driver = "libultra compressed sequence player";
    profile.controller = true;
    profile.eeprom = n64::EepromType::k4Kbit;

    // The audio thread keeps a second buffer waiting in the AI whenever the game thread runs, so no point in the game
    // thread is quiet. The snapshot comes in the audio frame instead, during the screens before the title, when no
    // song plays. The patches make the music update load the requested song at the end of that frame. The selector
    // sets the request. The game thread stops, so that the game requests no other song.
    profile.snapshot_address = kAudioFrameCommands;
    profile.snapshot_hit = 300;
    profile.quiet_tries = 120;
    profile.boot_seconds = 20.0;
    profile.selector.kind = TrackSelector::Kind::kRdram;
    profile.selector.address = kSongRequest;
    profile.selector.size = 4;
    profile.rdram_patches = {{.address = kSongPending, .words = {1}}, {.address = kSongDelay, .words = {0}}};
    profile.stop_threads = {kGameThread};

    profile.loop_watches.push_back({.address = kLoopJump, .target_register = 25, .track_register = 11});

    for (int index = 0; index < kSongs; index++)
    {
        Track track;
        track.number = static_cast<uint32_t>(index);
        const char* name = TrackName(index);
        char fallback[32];
        std::snprintf(fallback, sizeof(fallback), "Song 0x%02X", index);
        track.name = name != nullptr ? name : fallback;
        profile.tracks.push_back(track);
    }

    for (const Fanfare& fanfare : kFanfares)
    {
        Track track;
        track.number = 0x100 + fanfare.effect;
        track.name = fanfare.name;
        track.length_seconds = fanfare.seconds;
        track.rdram_patches = {
            {.address = kSongPending, .words = {0}},
            {.address = kEffectTablePointer, .words = {kRaceEffects}},
            {.address = kRaceEffects + kEffectSize * fanfare.effect + 4, .words = {kEffectRate}},
            {.address = kSoundState0, .words = {fanfare.sound, 0x00050000, 0x3F800000, 0, 0x7FFF4000}},
            {.address = kSoundQueue, .words = {0, 0xFFFFFFFF, fanfare.effect, fanfare.volume, 0x40, 0xBF800000, 0, 1}},
        };
        profile.tracks.push_back(track);
    }

    return profile;
}

} // namespace usf::rip::games
