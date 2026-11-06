// SPDX-License-Identifier: MIT

// Paper Mario (USA): the game's music. The game has no sound test. Each title names the place or the event where the
// game plays the song. A song holds up to four versions, which the game picks with a number when it starts the song.
//
// Addresses come from the game's code in the image and from runs in usf's core.
//
// - The game writes "IS64" to cartridge space at boot, in the function at 0x80025C60, for a debugging board. With the
//   core's legacy timing, the read after that returns the written word. On the hardware, that word has gone by then.
//   The read at 0x8002D1C4 computes the address of the code that sets up the audio. The profile therefore turns the
//   function at 0x80025C60 into a return in the ROM, and changes the header's checksums to suit.
// - The function at 0x800558D4 loads a song on a player and returns the song's name. The function at 0x80055970 takes
//   the name and a version, and starts the song.
// - The snapshot takes over the game thread at the first call of the function at 0x8014AC08. The audio engine is
//   running by then, and the game hasn't started any song.
// - The game's map scripts change five songs while they play, and each change is a track here. Bowser's Castle sets its
//   track levels in scripts at 0xA4C178 to 0xAA9768 in the ROM. The set for the castle's outside matches the song's
//   start, so it has no track. The code at 0x892A2C and 0x89FECC does the same for two rooms of the tunnels, and the
//   code at 0xB2C47C for Yoshi's Village. The code at 0x954004 swaps Dry Dry Desert's linked tracks at the oasis. Toad
//   Town's triggers, at the addresses in kChanges, pick a mix by the player's position. Five of them have a building
//   whose script sets the full level while the player is inside it.
// - The function at 0x8004EA34 runs the jumps of a song's list of phrases. A jump's count of 0 repeats for ever. The
//   list holds the jump's label in t0. At 0x8004EAB8 the function makes a jump's first pass, with the count in a1. At
//   0x8004EA68 it makes a later pass, with the count left in v0. A song without such a jump stops at the end of its
//   list.

#include <cstdio>

#include "rip/driver.h"
#include "rip/games/games.h"

namespace usf::rip::games
{

namespace
{

constexpr uint32_t kFirstSongSetup = 0x8014AC08;
constexpr uint32_t kLoadSong = 0x800558D4;
constexpr uint32_t kPlaySong = 0x80055970;
constexpr uint32_t kLoopFirstPass = 0x8004EAB8;
constexpr uint32_t kLoopLaterPass = 0x8004EA68;

// A track's number holds the song in its low byte and the version above it.
constexpr int kVersionShift = 8;
constexpr uint16_t kVersionMask = 0xFF;
constexpr uint16_t kSongMask = 0xFF;

// A track's number also holds a change above the version.
constexpr int kChangeShift = 16;
constexpr uint32_t kChangeTrackMask = 0xFFFF00FF;

constexpr uint32_t kSetLinkedMode = 0x80055CC4;
constexpr uint32_t kSetTrackVolumes = 0x80055DDC;
constexpr uint32_t kSetMixNear = 0x80056044;
constexpr uint32_t kSetMixFull = 0x80056068;

// The track of the third version of song 0x47 has a fixed length. A legacy player stops producing samples before the
// version's last sound ends, and the length stays short of that point.
constexpr uint32_t kShortTrack = (2 << kVersionShift) | 0x47;
constexpr double kShortTrackSeconds = 2.2;

// Each song's name in the game's sound archive. The change functions take it.
constexpr uint32_t kToadTown = 0x32313020;
constexpr uint32_t kDryDryDesert = 0x32303220;
constexpr uint32_t kYoshisVillage = 0x31343220;
constexpr uint32_t kTunnels = 0x33303420;
constexpr uint32_t kBowsersCastle = 0x31373020;

// The game waits a few frames after starting a song before it sets the track levels. A song's first commands would undo
// them. The driver makes those calls in each of the first frames. Linked tracks appear only in a song's later phrases,
// where the first pass of the loop starts. A swap needs them there, and repeating the call has no effect after that.
constexpr uint32_t kChangeFrames = 6;
constexpr int kFieldsPerFrame = 2;

struct Change
{
    uint32_t song;
    uint32_t change;
    uint32_t song_name;
    uint32_t function;
    uint32_t argument;
    bool at_start;
    const char* title;
};

constexpr Change kChanges[] = {
    {0x00, 1, kToadTown, kSetMixFull, 1, true, "Tayce T.'s House"}, // Trigger at 0x8313E0.
    {0x00, 2, kToadTown, kSetMixFull, 2, true, "Merlon's House"},   // Trigger at 0x8090E0.
    {0x00, 3, kToadTown, kSetMixFull, 3, true, "the Dojo"},         // Trigger at 0x7F9118.
    {0x00, 4, kToadTown, kSetMixNear, 4, true, "mix 4"},            // Trigger at 0x8313F8.
    {0x00, 5, kToadTown, kSetMixNear, 5, true, "mix 5"},            // Trigger at 0x809110.
    {0x00, 7, kToadTown, kSetMixFull, 7, true, "Russ T.'s House"},  // Trigger at 0x7F9100.
    {0x00, 8, kToadTown, kSetMixNear, 8, true, "mix 8"},            // Trigger at 0x8090F8.
    {0x00, 9, kToadTown, kSetMixFull, 9, true, "the Warehouse"},    // Trigger at 0x8520A0.
    {0x17, 1, kDryDryDesert, kSetLinkedMode, 1, false, "the Oasis"},
    {0x26, 1, kYoshisVillage, kSetTrackVolumes, 0, false, "full levels"},
    {0x4B, 1, kTunnels, kSetTrackVolumes, 2, false, "cold rooms"},
    {0x64, 1, kBowsersCastle, kSetTrackVolumes, 5, false, "first areas"},
    {0x64, 2, kBowsersCastle, kSetTrackVolumes, 6, false, "middle areas"},
    {0x64, 3, kBowsersCastle, kSetTrackVolumes, 7, false, "final areas"},
};

// A bus's effect changes only through a song's commands, and nothing resets it between songs. A song that sets no
// effect keeps the effect of the song before it. At the snapshot the bus holds no effect, and with none it mixes its
// send straight into the output. Such a song would play too loud and without reverb. This word in the sound globals
// requests an effect for the first bus, the same request as a song's command. The audio engine then applies it.
constexpr uint32_t kEffectRequest = 0x801D57E0;
constexpr uint32_t kEffectRequested = 0x00010000;
constexpr int kEffectShift = 24;

struct StartEffect
{
    uint32_t number;
    uint32_t effect;
};

constexpr uint32_t Number(uint32_t song, uint32_t version)
{
    return ((version - 1) << kVersionShift) | song;
}

// The game gives these no single value, and each one is a choice.
// - Normal Battle follows any field song. Script starts of field songs set effect 5 most often: 259, to 205 for
//   effect 1.
// - Raphael Raven plays in Deep Jungle, whose first version sets effect 2.
// - Four versions set their effect in a later phrase, and they start with that effect: 0x70, 0x79, 0x83 and 0x91.
// - The rest are later versions of a song, which no script starts with a constant. They start with the effect of the
//   song's first version.
constexpr StartEffect kStartEffects[] = {
    {Number(0x01, 2), 1},  {Number(0x02, 1), 5},  {Number(0x02, 2), 5},  {Number(0x02, 3), 5},  {Number(0x02, 4), 5},
    {Number(0x05, 3), 5},  {Number(0x0E, 2), 10}, {Number(0x14, 3), 5},  {Number(0x19, 2), 2},  {Number(0x1B, 3), 2},
    {Number(0x1C, 2), 10}, {Number(0x1E, 3), 5},  {Number(0x24, 2), 1},  {Number(0x25, 2), 2},  {Number(0x28, 1), 2},
    {Number(0x28, 3), 2},  {Number(0x2A, 2), 2},  {Number(0x2C, 2), 10}, {Number(0x37, 2), 5},  {Number(0x39, 2), 10},
    {Number(0x3F, 3), 1},  {Number(0x48, 2), 1},  {Number(0x49, 2), 1},  {Number(0x50, 2), 10}, {Number(0x56, 2), 10},
    {Number(0x57, 2), 5},  {Number(0x5A, 2), 5},  {Number(0x5B, 3), 1},  {Number(0x60, 2), 1},  {Number(0x66, 3), 5},
    {Number(0x67, 2), 5},  {Number(0x6D, 3), 5},  {Number(0x6E, 2), 10}, {Number(0x70, 2), 10}, {Number(0x79, 1), 1},
    {Number(0x7A, 2), 10}, {Number(0x7D, 2), 1},  {Number(0x7F, 2), 10}, {Number(0x80, 2), 1},  {Number(0x83, 2), 5},
    {Number(0x91, 2), 5},  {Number(0x96, 3), 10},
};

// The second version of this song lists the same phrases as the first.
constexpr uint32_t kRepeatedVersion = (1 << kVersionShift) | 0x61;

// The end of RDRAM. A run of the game holds zeros from 0x8038A978 to the end.
constexpr uint32_t kDriverAddress = 0x803FF000;

struct Song
{
    uint32_t number;
    uint32_t versions;
    const char* name;
};

// The songs in the game's list. Many entries in the list repeat Goomba Village, and the set holds it once.
constexpr Song kSongs[] = {
    {0x00, 2, "Toad Town"},
    {0x01, 2, "Goomba Village"},
    {0x02, 4, "Normal Battle"},
    {0x03, 3, "Special Battle"},
    {0x04, 2, "Jr. Troopa Battle"},
    {0x05, 3, "Final Bowser Battle"},
    {0x07, 2, "Goomba King Battle"},
    {0x08, 2, "Koopa Bros Battle"},
    {0x09, 2, "Fake Bowser Battle"},
    {0x0A, 2, "Tutankoopa Battle"},
    {0x0B, 3, "Tubba Blubba Battle"},
    {0x0C, 2, "General Guy Battle"},
    {0x0D, 3, "Lava Piranha Battle"},
    {0x0E, 2, "Huff N. Puff Battle"},
    {0x0F, 2, "Crystal King Battle"},
    {0x11, 1, "Pleasant Path"},
    {0x12, 2, "Fuzzy Attack"},
    {0x13, 2, "Koopa Village"},
    {0x14, 3, "Koopa Fortress"},
    {0x15, 2, "Dry Dry Outpost"},
    {0x16, 1, "Mt. Rugged"},
    {0x17, 1, "Dry Dry Desert"},
    {0x18, 2, "Dry Dry Ruins"},
    {0x19, 2, "Ruins Basement"},
    {0x1A, 2, "Forever Forest"},
    {0x1B, 3, "Boo's Mansion"},
    {0x1C, 2, "Cheerful Boo's Mansion"},
    {0x1D, 1, "Gusty Gulch"},
    {0x1E, 3, "Tubba's Manor"},
    {0x1F, 2, "Tubba Escape"},
    {0x20, 1, "Shy Guy Toybox"},
    {0x21, 1, "Toybox Train"},
    {0x22, 1, "Creepy Toybox"},
    {0x24, 2, "Jade Jungle"},
    {0x25, 2, "Deep Jungle"},
    {0x26, 3, "Yoshi's Village"},
    {0x27, 2, "Yoshi's Panic"},
    {0x28, 3, "Raphael Raven"},
    {0x29, 3, "Mt. Lavalava"},
    {0x2A, 2, "Volcano Escape"},
    {0x2B, 4, "Star Way Opens"},
    {0x2C, 2, "Master Battle"},
    {0x2D, 1, "Radio Island Sounds"},
    {0x2E, 1, "Radio Hot Hits"},
    {0x2F, 1, "Radio Golden Oldies"},
    {0x30, 2, "Flower Fields Cloudy"},
    {0x31, 2, "Flower Fields Sunny"},
    {0x32, 1, "Cloudy Climb"},
    {0x33, 2, "Puff Puff Machine"},
    {0x34, 2, "Sun Tower Cloudy"},
    {0x35, 2, "Sun Tower Sunny"},
    {0x37, 2, "Crystal Palace"},
    {0x38, 1, "Shiver City"},
    {0x39, 2, "Penguin Mystery"},
    {0x3A, 1, "Shiver Snowfield"},
    {0x3B, 1, "Shiver Mountain"},
    {0x3C, 1, "Starborn Valley"},
    {0x3D, 1, "Merlar Theme"},
    {0x3E, 1, "Mail Call"},
    {0x3F, 3, "Peach's Castle Party"},
    {0x40, 2, "Chapter End"},
    {0x41, 3, "Chapter Start"},
    {0x42, 2, "Item Upgrade"},
    {0x44, 1, "Phonograph Music"},
    {0x45, 1, "Tutankoopa Theme"},
    {0x46, 2, "Kammy Koopa Theme"},
    {0x47, 3, "Jr. Troopa Theme"},
    {0x48, 2, "Bullet Bill Assault"},
    {0x49, 2, "Monty Mole Assault"},
    {0x4A, 2, "Shy Guy Invasion"},
    {0x4B, 2, "Toad Town Tunnels"},
    {0x4C, 2, "Whale Theme"},
    {0x4D, 1, "Forever Forest Warning"},
    {0x4E, 2, "Yoshi Kids Found"},
    {0x4F, 1, "Unused Fanfare"},
    {0x50, 2, "Goomba King Theme"},
    {0x51, 1, "Koopa Bros Interlude"},
    {0x52, 4, "Koopa Bros Theme"},
    {0x53, 1, "Tutankoopa Warning"},
    {0x54, 2, "Tutankoopa Revealed"},
    {0x55, 2, "Tubba Blubba Theme"},
    {0x56, 2, "General Guy Theme"},
    {0x57, 2, "Lava Piranha Theme"},
    {0x58, 2, "Huff N. Puff Theme"},
    {0x59, 1, "Crystal King Theme"},
    {0x5A, 2, "Blooper Theme"},
    {0x5B, 3, "Miniboss Battle"},
    {0x5C, 2, "Monstar Theme"},
    {0x5D, 2, "Club 64"},
    {0x5E, 1, "Unused Opening"},
    {0x5F, 1, "Bowser's Castle Falls"},
    {0x60, 2, "Star Haven"},
    {0x61, 3, "Shooting Star Summit"},
    {0x62, 3, "Starship Theme"},
    {0x63, 3, "Star Sanctuary"},
    {0x64, 3, "Bowser's Castle"},
    {0x65, 2, "Bowser's Castle Caves"},
    {0x66, 3, "Bowser Theme"},
    {0x67, 2, "Bowser Battle"},
    {0x68, 2, "Peach Wishes"},
    {0x69, 2, "File Select"},
    {0x6A, 3, "Main Theme"},
    {0x6B, 3, "Bowser Attacks"},
    {0x6C, 1, "Mario Falls"},
    {0x6D, 3, "Peach Appears"},
    {0x6E, 2, "The End"},
    {0x6F, 1, "Recovered Star Rod"},
    {0x70, 3, "Twink Theme"},
    {0x71, 1, "Stirring Cake"},
    {0x72, 1, "Gourmet Guy Freakout"},
    {0x73, 2, "Prisoner Peach Theme"},
    {0x74, 2, "Peach Mission"},
    {0x75, 2, "Peach Sneaking"},
    {0x76, 1, "Peach Caught"},
    {0x77, 3, "Peach Quiz Intro"},
    {0x78, 2, "Star Spirit Theme"},
    {0x79, 3, "Penguin Whodunit"},
    {0x7A, 2, "Penguin Wakes Up"},
    {0x7B, 3, "Magic Beanstalk"},
    {0x7C, 3, "Merlee Spell"},
    {0x7D, 2, "Lakilester Theme"},
    {0x7E, 3, "Goomba Bros Retreat"},
    {0x7F, 2, "Sunshine Returns"},
    {0x80, 2, "Riding the Rails"},
    {0x81, 2, "Riding the Whale"},
    {0x82, 2, "New Partner"},
    {0x83, 2, "Dry Dry Ruins Appear"},
    {0x84, 1, "Candy Canes"},
    {0x85, 1, "Playroom"},
    {0x86, 2, "Moustafa Theme"},
    {0x87, 1, "Game Over"},
    {0x88, 1, "Taking Rest"},
    {0x89, 4, "Flower NPC Theme"},
    {0x8A, 1, "Flower Gate Appears"},
    {0x8B, 2, "Battle End"},
    {0x8C, 2, "Pop Diva Song"},
    {0x8D, 2, "Boo Minigame"},
    {0x8E, 2, "Level Up"},
    {0x90, 2, "Parade Day"},
    {0x91, 2, "Parade Night"},
    {0x94, 1, "Mario Bros House"},
    {0x95, 4, "Intro Story"},
    {0x96, 3, "New Partner (Japanese)"},
};

} // namespace

GameProfile NusNmqeUsa()
{
    GameProfile profile;
    profile.game = "Paper Mario";
    profile.serial = "NUS-NMQE-USA";
    profile.sha1 = "3837f44cda784b466c9a2d99df70d77c322b97a0";
    profile.year = "2001";
    profile.cic = n64::Cic::k6103;
    profile.sound_driver = "Paper Mario BGM player";
    profile.controller = true;

    // A return in the function that writes to cartridge space, and the checksums of the ROM with it.
    profile.rom_patches.push_back({.offset = 0x10, .bytes = {0xBD, 0xE3, 0x77, 0x9B, 0x94, 0x92, 0x26, 0xEA}});
    profile.rom_patches.push_back({.offset = 0x1060, .bytes = {0x03, 0xE0, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00}});

    profile.snapshot_address = kFirstSongSetup;
    profile.snapshot_hit = 1;
    profile.quiet_tries = 1;
    profile.boot_seconds = 3.0;

    CallDriverOptions driver;
    driver.address = kDriverAddress;
    driver.start_calls = {
        {.function = kLoadSong, .arguments = {{.track = true, .mask = kSongMask}, {.value = 0}}},
        {.function = kPlaySong,
         .arguments = {{.result = true}, {.track = true, .right_shift = kVersionShift, .mask = kVersionMask}}}};
    for (const Change& change : kChanges)
    {
        GameCall call;
        call.function = change.function;
        call.arguments = {{.value = change.song_name}, {.value = change.argument}};
        call.track_mask = kChangeTrackMask;
        call.track_value = (change.change << kChangeShift) | change.song;
        if (change.at_start)
        {
            driver.start_calls.push_back(call);
        }
        else
        {
            call.frames = change.function == kSetLinkedMode ? 0 : kChangeFrames;
            driver.frame_calls.push_back(call);
        }
    }

    driver.fields = kFieldsPerFrame;
    profile.driver = BuildCallDriver(driver);

    profile.loop_watches.push_back(
        {.address = kLoopFirstPass, .target_register = 8, .match_register = 5, .match_value = 0});
    profile.loop_watches.push_back(
        {.address = kLoopLaterPass, .target_register = 8, .match_register = 2, .match_value = 0});

    for (const Song& song : kSongs)
    {
        for (uint32_t version = 0; version < song.versions; version++)
        {
            Track track;
            track.number = (version << kVersionShift) | song.number;
            if (track.number == kRepeatedVersion)
            {
                continue;
            }

            char name[96];
            if (version == 0)
            {
                std::snprintf(name, sizeof(name), "Song 0x%02X: %s", song.number, song.name);
            }
            else
            {
                std::snprintf(name, sizeof(name), "Song 0x%02X: %s, version %u", song.number, song.name, version + 1);
            }
            track.name = name;

            for (const StartEffect& effect : kStartEffects)
            {
                if (effect.number == track.number)
                {
                    track.rdram_patches.push_back(
                        {.address = kEffectRequest, .words = {(effect.effect << kEffectShift) | kEffectRequested}});
                }
            }
            if (track.number == kShortTrack)
            {
                track.length_seconds = kShortTrackSeconds;
            }
            profile.tracks.push_back(track);
        }

        for (const Change& change : kChanges)
        {
            if (change.song == song.number)
            {
                Track track;
                track.number = (change.change << kChangeShift) | song.number;
                char name[96];
                std::snprintf(name, sizeof(name), "Song 0x%02X: %s, %s", song.number, song.name, change.title);
                track.name = name;
                profile.tracks.push_back(track);
            }
        }
    }

    return profile;
}

} // namespace usf::rip::games
