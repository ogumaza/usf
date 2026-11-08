// SPDX-License-Identifier: MIT

// Super Mario 64 (USA): the game's music, sequences 0x01 to 0x22 of the sequence table at cartridge offset 0x7B0860.
// Sequence 0 is the sound-effect player. nus_nsme_usa.cpp has the sound effects, in a set of their own.
//
// Addresses come from the game's code in the image and from runs in usf's core.
//
// - The play-music function at 0x803219AC takes a sequence player in a0, the sequence in the low byte of a1 and a
//   priority in its high byte, and a fade-in in a2. For player 0, the level's music, it keeps a queue by priority and
//   starts the sequence at the volume that the table at 0x80333188 gives it (through 0x80320F68). For another player
//   it starts the sequence through 0x80320544 at the sequence's own volume. Bit 7 of the sequence is its variation,
//   which the sequence's script reads.
// - The sound reset at 0x80322230 takes an audio set-up in a0, 0 to 7, from the table at 0x803332A0. The set-ups
//   differ in their reverb and their number of voices. The levels' scripts start their music through 0x80249178, which
//   resets the sound with the area's set-up and calls the play-music function for player 0. Set-up 7 is the credits'.
// - The game's code starts the jingles on player 1, through the functions at 0x80322078 to 0x803221F4 and through the
//   play-music function at 0x80258250. It starts the piranha plants' lullaby, the endless stairs and the merry-go-round
//   on player 1 too, through 0x80321E48, which can also set player 1's volume through 0x8031E710. Near the
//   merry-go-round the volume is 78 (the call at 0x802C5C10).
// - The game-loop tick at 0x8031FD84 takes no arguments. The sound thread then runs 0x803208EC once, which fades the
//   channels of player 0 by the level and by where Mario is. The table of the levels' rules is at 0x80332FC4, by the
//   level at 0x8032DDF8. A level's rules read Mario's position at 0x8033B1AC, the area at 0x8033BACA and the room at
//   0x80361250. They pick an entry of the table at 0x80333060, which fades sets of channels in or out. The first entry
//   after a sequence starts takes effect at once. Only Dire, Dire Docks, Jolly Roger Bay, Big Boo's Haunt, Hazy Maze
//   Cave and Wet-Dry World have rules. A track that needs them gets its level and Mario's place in its mini.
// - The sound reset at 0x80322230 returns to 0x802491D8 in the game thread. The first return comes before any music.
// - The sequence player's script reader handles its jumps (commands 0xF5 and 0xF9 to 0xFB) at 0x8031DDD0 to
//   0x8031DE2C. At 0x8031DE28 the new script address is in t7 and the sequence player in s1. Players 0 and 1 are at
//   0x80222618 and 0x80222758.
//
// A track's number holds what the driver passes to the game's functions: the sequence in bits 0 to 7, the audio
// set-up in bits 8 to 10, a sound reset in bit 11, player 1 in bit 12, a volume for player 1 in bit 13 with the volume
// in bits 16 to 23, and a number that tells apart the tracks of one sequence in bits 24 to 27.

#include <cstdio>

#include "rip/driver.h"
#include "rip/games/games.h"

namespace usf::rip::games
{

namespace
{

constexpr uint32_t kPlayMusic = 0x803219AC;
constexpr uint32_t kSoundReset = 0x80322230;
constexpr uint32_t kSetPlayerVolume = 0x8031E710;
constexpr uint32_t kGameLoopTick = 0x8031FD84;
constexpr uint32_t kAfterSoundReset = 0x802491D8;
constexpr uint32_t kSequenceJump = 0x8031DE28;
constexpr uint32_t kPlayer0 = 0x80222618;
constexpr uint32_t kPlayer1 = 0x80222758;

// The words that the levels' rules read. The level and the room are the upper halves of their words, and the area
// the lower half of its word. At the snapshot the other halves hold 0.
constexpr uint32_t kLevelWord = 0x8032DDF8;
constexpr uint32_t kAreaWord = 0x8033BAC8;
constexpr uint32_t kRoomWord = 0x80361250;
constexpr uint32_t kMarioY = 0x8033B1B0;

// The last lines of the third frame buffer, as in the sound effects' profile. Only the RDP writes there, and it draws
// nothing after the snapshot.
constexpr uint32_t kDriverAddress = 0x803FF000;

constexpr uint32_t kVariation = 0x80;
constexpr uint32_t kReset = 1u << 11;
constexpr uint32_t kPlayer1Flag = 1u << 12;
constexpr uint32_t kVolumeFlag = 1u << 13;

// The levels with rules, and the float 100.0 for Mario's height.
constexpr uint32_t kLevelBigBoosHaunt = 4;
constexpr uint32_t kLevelHazyMazeCave = 7;
constexpr uint32_t kLevelWetDryWorld = 11;
constexpr uint32_t kLevelDireDireDocks = 23;
constexpr uint32_t kHeight100 = 0x42C80000;
constexpr uint32_t kHeightMinus300 = 0xC3960000;

// A place in a level: the level, the area, the room and Mario's height. 0 leaves the snapshot's value.
struct Place
{
    uint32_t level = 0;
    uint32_t area = 0;
    uint32_t room = 0;
    uint32_t height = 0;
};

struct Piece
{
    uint32_t sequence; // With the variation in bit 7.
    uint32_t setup;    // The audio set-up of the sound reset, 0 for the snapshot's.
    bool player1;
    uint32_t volume;  // Player 1's volume, or 0 for the sequence's.
    uint32_t variant; // Tells apart the tracks of one sequence.
    Place place;
    const char* name;
};

// Each mini that needs the levels' rules puts Mario in one place, which picks its arrangement. At the snapshot, Mario
// is at (0, 0, 0) in area 1 and room 0, and the level is 1, which has no rules. The rules and the places:
// - Dire, Dire Docks (level 23). In area 1 with x under -800, or with y of -2000 or more and x under 470, channels 0,
//   1, 6, 9, 10 and 11 fade out. In area 2 with y of 100 or more, every channel plays. Anywhere else, channels 6, 9, 10
//   and 11 fade out. The minis use area 1 at (0, 0, 0), area 2 at (0, 0, 0) and area 2 at a height of 100. Jolly Roger
//   Bay's rules pick the same three arrangements with the same set-up, so they make no other tracks.
// - Big Boo's Haunt (level 4). In rooms 10 and 13, channels 4 to 6 fade out. Elsewhere every channel plays. The minis
//   use room 0 and room 13.
// - Hazy Maze Cave (level 7). Channel 3 fades out where x is 0 or more and y is under -203, and where x is under 0 and
//   y is under -2400. Channel 8 fades out elsewhere. The minis use (0, 0, 0) and a height of -300.
// - Wet-Dry World (level 11). Channel 3 fades out in area 2, and in area 1 below a height of -670. Channel 8 fades out
//   elsewhere. The minis use area 1 at (0, 0, 0) and area 2.
// The other levels that play sequence 0x0C have no rules, and every channel plays there: the Shifting Sand Land
// pyramid, the igloo of Snowman's Land, Tiny-Huge Island's inside and the Cavern of the Metal Cap.
//
// The set-ups follow the levels' scripts. The jingles and the cap and event music play in the set-up of whatever level
// the player is in. Those tracks use set-up 0, the set-up of most levels, unless they play in one place only. Set-ups 5
// and 6 have the reverbs of 0 and 1, so 0 and 1 also stand for the levels with set-ups 5 and 6.
// clang-format off
const Piece kPieces[] = {
    {0x01, 0, true,  0,  0, {}, "Sequence 0x01: star collected"},
    {0x02, 0, false, 0,  0, {}, "Sequence 0x02: title screen"},
    {0x02 | kVariation, 0, false, 0, 0, {}, "Sequence 0x02: title screen, with the variation, after a game over"},
    {0x03, 0, false, 0,  0, {}, "Sequence 0x03: grass levels"},
    {0x04, 1, false, 0,  0, {}, "Sequence 0x04: inside the castle"},
    {0x05, 3, false, 0,  0, {.level = kLevelDireDireDocks, .area = 1},
     "Sequence 0x05: water levels, without channels 0, 1, 6, 9, 10 and 11"},
    {0x05, 3, false, 0,  1, {.level = kLevelDireDireDocks, .area = 2},
     "Sequence 0x05: water levels, without channels 6, 9, 10 and 11"},
    {0x05, 3, false, 0,  2, {.level = kLevelDireDireDocks, .area = 2, .height = kHeight100},
     "Sequence 0x05: water levels, all channels"},
    {0x05 | kVariation, 3, false, 0, 0, {}, "Sequence 0x05: water levels, with the variation, in the Secret Aquarium"},
    {0x06, 0, false, 0,  0, {}, "Sequence 0x06: hot levels"},
    {0x06, 4, false, 0,  1, {}, "Sequence 0x06: hot levels, inside the volcano"},
    {0x07, 2, false, 0,  0, {}, "Sequence 0x07: Bowser's battles"},
    {0x08, 0, false, 0,  0, {}, "Sequence 0x08: snow levels"},
    {0x09, 1, false, 0,  0, {}, "Sequence 0x09: slides"},
    {0x09, 0, false, 0,  1, {}, "Sequence 0x09: slides, in Rainbow Ride and three secret courses"},
    {0x0A, 6, false, 0,  0, {.level = kLevelBigBoosHaunt}, "Sequence 0x0A: Big Boo's Haunt, indoors"},
    {0x0A, 6, false, 0,  1, {.level = kLevelBigBoosHaunt, .room = 13}, "Sequence 0x0A: Big Boo's Haunt, outdoors"},
    {0x0B, 0, true,  0,  0, {}, "Sequence 0x0B: piranha plant's lullaby"},
    {0x0C, 4, false, 0,  0, {}, "Sequence 0x0C: underground levels, all channels"},
    {0x0C, 4, false, 0,  1, {.level = kLevelHazyMazeCave}, "Sequence 0x0C: underground levels, without channel 8"},
    {0x0C, 4, false, 0,  2, {.level = kLevelHazyMazeCave, .height = kHeightMinus300},
     "Sequence 0x0C: underground levels, without channel 3"},
    {0x0C, 3, false, 0,  3, {.level = kLevelWetDryWorld, .area = 1},
     "Sequence 0x0C: underground levels, without channel 8, in Wet-Dry World"},
    {0x0C, 3, false, 0,  4, {.level = kLevelWetDryWorld, .area = 2},
     "Sequence 0x0C: underground levels, without channel 3, in Wet-Dry World"},
    {0x0D, 0, false, 0,  0, {}, "Sequence 0x0D: star select"},
    {0x0E, 0, false, 0,  0, {}, "Sequence 0x0E: wing cap"},
    {0x0F, 0, false, 0,  0, {}, "Sequence 0x0F: metal cap"},
    {0x10, 0, true,  0,  0, {}, "Sequence 0x10: Bowser's message"},
    {0x11, 0, false, 0,  0, {}, "Sequence 0x11: Bowser's roads"},
    {0x12, 0, true,  0,  0, {}, "Sequence 0x12: high score"},
    {0x13, 6, true,  78, 0, {}, "Sequence 0x13: merry-go-round"},
    {0x14, 0, true,  0,  0, {}, "Sequence 0x14: race fanfare"},
    {0x15, 0, true,  0,  0, {}, "Sequence 0x15: star appears"},
    {0x16, 0, false, 0,  0, {}, "Sequence 0x16: boss"},
    {0x17, 2, true,  0,  0, {}, "Sequence 0x17: key collected"},
    {0x18, 1, true,  0,  0, {}, "Sequence 0x18: endless stairs"},
    {0x19, 2, false, 0,  0, {}, "Sequence 0x19: final battle"},
    {0x1A, 7, false, 0,  0, {}, "Sequence 0x1A: credits"},
    {0x1B, 0, true,  0,  0, {}, "Sequence 0x1B: puzzle solved"},
    {0x1C, 1, true,  0,  0, {}, "Sequence 0x1C: Toad's message"},
    {0x1D, 0, false, 0,  0, {}, "Sequence 0x1D: Peach's message"},
    {0x1E, 0, false, 0,  0, {}, "Sequence 0x1E: intro"},
    {0x1F, 2, false, 0,  0, {}, "Sequence 0x1F: Grand Star collected"},
    {0x20, 0, false, 0,  0, {}, "Sequence 0x20: ending"},
    {0x21, 0, false, 0,  0, {}, "Sequence 0x21: file select"},
    {0x22, 0, false, 0,  0, {}, "Sequence 0x22: Lakitu's message"},
};
// clang-format on

uint32_t TrackNumber(const Piece& piece)
{
    uint32_t number = piece.sequence | (piece.setup << 8) | (piece.variant << 24);
    if (piece.setup != 0)
    {
        number |= kReset;
    }
    if (piece.player1)
    {
        number |= kPlayer1Flag;
    }
    if (piece.volume != 0)
    {
        number |= kVolumeFlag | (piece.volume << 16);
    }

    return number;
}

std::vector<RdramPatch> PlacePatches(const Place& place)
{
    std::vector<RdramPatch> patches;
    if (place.level != 0)
    {
        patches.push_back({.address = kLevelWord, .words = {place.level << 16}});
    }
    if (place.area != 0)
    {
        patches.push_back({.address = kAreaWord, .words = {place.area}});
    }
    if (place.room != 0)
    {
        patches.push_back({.address = kRoomWord, .words = {place.room << 16}});
    }
    if (place.height != 0)
    {
        patches.push_back({.address = kMarioY, .words = {place.height}});
    }

    return patches;
}

} // namespace

GameProfile NusNsmeUsaMusic()
{
    GameProfile profile;
    profile.game = "Super Mario 64";
    profile.serial = "NUS-NSME-USA";
    profile.sha1 = "9bef1128717f958171a4afac3ed78ee2bb4e86ce";
    profile.set = "music";
    profile.year = "1996";
    profile.cic = n64::Cic::k6102;
    profile.sound_driver = "Super Mario 64 sound engine";

    // The driver takes over the game thread at the first return from the sound reset, as in the sound effects'
    // profile. It resets the sound with the track's set-up, unless the track uses the snapshot's set-up 0. It then
    // starts the sequence on its player, and sets player 1's volume where the game does. Each frame ends with the
    // game-loop tick. A frame is two fields, as in the game.
    profile.snapshot_address = kAfterSoundReset;
    profile.snapshot_hit = 1;
    profile.quiet_tries = 30;
    profile.boot_seconds = 5.0;

    const CallArgument sequence = {.track = true, .mask = 0xFF};
    CallDriverOptions driver;
    driver.address = kDriverAddress;
    driver.start_calls = {
        {.function = kSoundReset,
         .arguments = {{.track = true, .right_shift = 8, .mask = 0x7}},
         .track_mask = kReset,
         .track_value = kReset},
        {.function = kPlayMusic,
         .arguments = {{.value = 0}, sequence, {.value = 0}},
         .track_mask = kPlayer1Flag,
         .track_value = 0},
        {.function = kPlayMusic,
         .arguments = {{.value = 1}, sequence, {.value = 0}},
         .track_mask = kPlayer1Flag,
         .track_value = kPlayer1Flag},
        {.function = kSetPlayerVolume,
         .arguments = {{.value = 1}, {.value = 1}, {.track = true, .right_shift = 16, .mask = 0xFF}},
         .track_mask = kVolumeFlag,
         .track_value = kVolumeFlag},
    };
    driver.frame_calls = {{.function = kGameLoopTick}};
    driver.fields = 2;
    profile.driver = BuildCallDriver(driver);

    // Players 0 and 1 run the music. Player 2 runs the sound effects' script, whose jumps don't mark the music's loops.
    profile.loop_watches.push_back({.address = kSequenceJump,
                                    .target_register = 15,
                                    .track_register = 17,
                                    .match_register = 17,
                                    .match_value = kPlayer0});
    profile.loop_watches.push_back({.address = kSequenceJump,
                                    .target_register = 15,
                                    .track_register = 17,
                                    .match_register = 17,
                                    .match_value = kPlayer1});

    for (const Piece& piece : kPieces)
    {
        Track track;
        track.number = TrackNumber(piece);
        track.name = piece.name;
        track.rdram_patches = PlacePatches(piece.place);
        profile.tracks.push_back(track);
    }

    return profile;
}

} // namespace usf::rip::games
