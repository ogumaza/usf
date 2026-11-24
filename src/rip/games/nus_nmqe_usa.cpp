// SPDX-License-Identifier: MIT

// Paper Mario (USA): the game's music. The game has no sound test. A song has up to four versions. The game picks one
// with a number when it starts the song.
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
//   start. It has no track. The code at 0x892A2C and 0x89FECC does the same for two rooms of the tunnels, and the code
//   at 0xB2C47C for Yoshi's Village. The code at 0x954004 swaps Dry Dry Desert's linked tracks at the oasis. Toad
//   Town's triggers, at the addresses in kChanges, pick a mix by the player's position. Six of them have a place whose
//   script sets the full level while the player is inside it: five buildings and the floor of Rowf's Badge Shop.
// - Outside such a place, its trigger sets the near level of the same mix. The set therefore has a track for each of
//   the six mixes at the near level and at the full level.
// - Toad Town's song is the only one with mixes. Its tracks 8 to 11 are a layer that plays only in the loop. Every 96
//   ticks, each of them branches to the section of the current mix in a table of ten. The sections of mix 0 are
//   empty. A trigger calls 0x80056028 (far, level 0), 0x80056044 (near, level 0x57) or 0x80056068 (full, level 0x7F).
//   Each of these calls the function at 0x800506C8. That function sets two flags on each track of the song: one when
//   the mix changes and one when the level changes. At the track's next branch (0x800501A8), the first flag sets the
//   track's level to 0 and resets its voices. The second starts a fade to the level of the mix over 144 ticks
//   (0x80050888). Track 0 runs a command that sets the level of the base tracks, 1 to 5, at the start of the loop and
//   every 192 ticks after it (0x800502F0). After a change of level, that command fades the base tracks over 72 ticks:
//   to level 63 at the full level, and to level 127 otherwise. The intro has no branch. A mix set during the intro
//   therefore comes in at the first branch of the loop. The driver calls 0x80056044 or 0x80056068 after it starts the
//   song and before the song's first tick. Each mix of the set then plays the fades from the first branch of the loop.
// - The script of Merlon's House requests the song, waits 20 frames and sets the full level. The script of the
//   Warehouse waits 20 frames and sets a flag. The trigger then sets the full level. Those calls come during the intro
//   too. The trigger of mix 8 is by Rowf's Badge Shop (Rowf's position at 0x815444). The record of that trigger, at
//   0x8090F8, contains the map flag 0xFAA2B58B (at 0x80910C). The trigger sets the full level while that flag is set.
//   The same flag marks the shop as open. When the player steps onto the shop's floor, the trigger at 0x816BA0 runs the
//   script at 0x8167E0. That script sets the flag at 0x816828 and runs the script at 0x815FBC. The second script sets
//   the flag again at 0x815FC4. When the player steps off, the trigger at 0x816BBC runs the script at 0x81686C. That
//   script clears the flag at 0x8168A4 and runs the script at 0x8163B0. The second script clears the flag again at
//   0x8163B8. No script sets the flag of mix 4 (at 0x83140C) or of mix 5 (at 0x809124). The trigger of mix 4 is by the
//   pipe to the Tunnels (the map's entry at 0x826820), and the trigger of mix 5 by Minh T.'s Garden (Minh T.'s position
//   at 0x81E4B0).
// - Mix 6 has notes in the layer. The game never selects it. Only the function at 0x8014AA54 calls the three mix
//   functions above. Four maps' triggers and one script call that function, and they name the other eight mixes. The
//   set has a track of mix 6 at the near level, as for mixes 4 and 5. The game sets no level for mix 6. That level is
//   therefore a choice.
// - The function at 0x8004EA34 runs the jumps of a song's list of phrases. A jump's count of 0 repeats for ever. The
//   list holds the jump's label in t0. At 0x8004EAB8 the function makes a jump's first pass, with the count in a1. At
//   0x8004EA68 it makes a later pass, with the count left in v0. A song without such a jump stops at the end of its
//   list.
// - The game thread (0x800B1B90, priority 50) makes the message queue at 0x800B42F0 at 0x8006026C, and adds it to the
//   scheduler's clients at 0x8006027C. At each retrace the scheduler sends a message to its clients, at 0x8005F0F8,
//   whether the game draws or not. The queue is empty at the snapshot. The game thread waits for its messages at
//   0x80060298, through osRecvMesg at 0x800656F0. Only two threads have a lower priority: the thread at 0x800A4420
//   (priority 10), in a jump to itself at 0x80026054, and the idle thread (0x800A4270, priority 0), in a branch to
//   itself at 0x8005E98C.

#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

#include "xsflib/bytes.h"

#include "rip/driver.h"
#include "rip/games/games.h"
#include "rip/games/tracks.h"
#include "rip/mips.h"

namespace usf::rip::games
{
namespace
{

constexpr uint32_t kFirstSongSetup = 0x8014AC08;
constexpr uint32_t kLoadSong = 0x800558D4;
constexpr uint32_t kPlaySong = 0x80055970;
constexpr uint32_t kRecvMesg = 0x800656F0;
constexpr uint32_t kRetraceQueue = 0x800B42F0;
constexpr uint32_t kLoopFirstPass = 0x8004EAB8;
constexpr uint32_t kLoopLaterPass = 0x8004EA68;

// The player that kLoadSong loads the song on, in a1: the first of the engine's two music players.
constexpr uint32_t kMusicPlayer = 0;

// A track's number contains the song in its low byte and the version above it.
constexpr int kVersionShift = 8;
constexpr uint16_t kVersionMask = 0xFF;
constexpr uint16_t kSongMask = 0xFF;

// A track's number also contains a change above the version.
constexpr int kChangeShift = 16;
constexpr uint32_t kChangeTrackMask = 0xFFFF00FF;

constexpr uint32_t kSetLinkedMode = 0x80055CC4;
constexpr uint32_t kSetTrackVolumes = 0x80055DDC;
constexpr uint32_t kSetMixNear = 0x80056044;
constexpr uint32_t kSetMixFull = 0x80056068;

// The number of the track of `version` of `song`, counting versions from 1.
constexpr uint32_t Number(uint32_t song, uint32_t version)
{
    return ((version - 1) << kVersionShift) | song;
}

// The track of the third version of song 0x47 has a fixed length. A legacy player stops producing samples before the
// version's last sound ends, and the length stays short of that point.
constexpr uint32_t kShortTrack = Number(0x47, 3);
constexpr double kShortTrackSeconds = 2.2;

// Each song's name in the game's sound archive. The change functions take it.
constexpr uint32_t kToadTown = 0x32313020;
constexpr uint32_t kDryDryDesert = 0x32303220;
constexpr uint32_t kYoshisVillage = 0x31343220;
constexpr uint32_t kTunnels = 0x33303420;
constexpr uint32_t kBowsersCastle = 0x31373020;

// The game waits a few frames after starting a song before it sets the track levels. A song's first commands would undo
// them. The driver makes those calls in each of the first frames. Linked tracks appear only in a song's later phrases,
// where the first pass of the loop starts. A swap requires them there, and repeating the call has no effect after that.
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
};

// The order of the rows gives the order of the tracks. A row keeps its place once a release contains it.
constexpr Change kChanges[] = {
    {0x00, 1, kToadTown, kSetMixFull, 1, true}, // Tayce T.'s House. Trigger at 0x8313E0.
    {0x00, 2, kToadTown, kSetMixFull, 2, true}, // Merlon's House. Trigger at 0x8090E0.
    {0x00, 3, kToadTown, kSetMixFull, 3, true}, // The Dojo. Trigger at 0x7F9118.
    {0x00, 4, kToadTown, kSetMixNear, 4, true}, // The pipe to the Tunnels. Trigger at 0x8313F8.
    {0x00, 5, kToadTown, kSetMixNear, 5, true}, // Minh T.'s Garden. Trigger at 0x809110.
    {0x00, 7, kToadTown, kSetMixFull, 7, true}, // Russ T.'s House. Trigger at 0x7F9100.
    {0x00, 8, kToadTown, kSetMixFull, 8, true}, // Rowf's Badge Shop. Trigger at 0x8090F8.
    {0x00, 9, kToadTown, kSetMixFull, 9, true}, // The Warehouse. Trigger at 0x8520A0.
    // The six places at the near level. The change is the mix plus 0x10.
    {0x00, 0x11, kToadTown, kSetMixNear, 1, true}, // Outside Tayce T.'s House.
    {0x00, 0x12, kToadTown, kSetMixNear, 2, true}, // Outside Merlon's House.
    {0x00, 0x13, kToadTown, kSetMixNear, 3, true}, // Outside the Dojo.
    {0x00, 0x17, kToadTown, kSetMixNear, 7, true}, // Outside Russ T.'s House.
    {0x00, 0x19, kToadTown, kSetMixNear, 9, true}, // Outside the Warehouse.
    {0x00, 0x18, kToadTown, kSetMixNear, 8, true}, // Outside Rowf's Badge Shop.
    // Mix 6. The game never plays it.
    {0x00, 6, kToadTown, kSetMixNear, 6, true},
    {0x17, 1, kDryDryDesert, kSetLinkedMode, 1, false},    // The Oasis.
    {0x26, 1, kYoshisVillage, kSetTrackVolumes, 0, false}, // Full levels.
    {0x4B, 1, kTunnels, kSetTrackVolumes, 2, false},       // Cold rooms.
    {0x64, 1, kBowsersCastle, kSetTrackVolumes, 5, false}, // First areas.
    {0x64, 2, kBowsersCastle, kSetTrackVolumes, 6, false}, // Middle areas.
    {0x64, 3, kBowsersCastle, kSetTrackVolumes, 7, false}, // Final areas.
};

// A bus's effect changes only through a song's commands, and nothing resets it between songs. A song that sets no
// effect keeps the effect of the song before it. At the snapshot the bus has no effect, and with none it mixes its send
// straight into the output. Such a song would play too loud and without reverb. This word in the sound globals requests
// an effect for the first bus, the same request as a song's command. The audio engine then applies it.
constexpr uint32_t kEffectRequest = 0x801D57E0;
constexpr uint32_t kEffectRequested = 0x00010000;
constexpr int kEffectShift = 24;

struct StartEffect
{
    uint32_t number;
    uint32_t effect;
};

// These versions set no effect at their start. Where the game starts a version after more than one song, or never
// starts it, the effect here is a choice.
// - Normal Battle keeps the effect of the field song that played before the battle. The function at 0x8014AEF8 pushes
//   the field song and starts the battle song. The battle song sets no effect. Of the game's areas with enemies, 6.5
//   play the battle after effect 1, the small room. Another 6.5 play it after effect 5, the echo, and 5 after effect 2
//   or 10, the big room. An area with two field songs counts half for each. The game's first battles come after effect
//   1, on the roads around Goomba Village and Koopa Village. The versions therefore start with effect 1.
//   kEffectGroups adds tracks for the other effects.
// - Every change of song or version is a fresh start: the function at 0x8014A548 stops the playing song, then loads and
//   starts the requested one. Only a request for the playing song and version leaves the song as it is (0x8014A7E0). A
//   version that sets no effect at its start therefore keeps the effect of the song that played before it.
// - The scripts start the first version of Raphael Raven at 0xB805F8 and 0xB80674 in the tree, at 0xB83860 when a save
//   in the tree loads, and at 0xB85C3C outside the tree. The game first starts it after Deep Jungle. Deep Jungle's song
//   sets effect 2. kEffectGroups adds tracks for the other states.
// - Four versions set their effect in a later phrase: 0x70, 0x79, 0x83 and 0x91. Three of them start after one song
//   only, and they start with that song's effect. The script at 0xAB426C starts version 2 of 0x70 after the script at
//   0xAB3320 starts song 0x78. Song 0x78 sets effect 10. The script at 0xD02568 starts version 1 of 0x79 in Shiver
//   City. Shiver City's song, 0x38, sets effect 10. The version sets effect 1 at 26 s. The script at 0x92D8EC starts
//   version 2 of 0x83 after Dry Dry Desert. Dry Dry Desert's song, 0x17, sets effect 1. The version sets effect 5 at
//   14 s.
// - No code starts the other versions in this table. They start with the effect of the song's first version.
constexpr StartEffect kStartEffects[] = {
    {Number(0x01, 2), 1},  {Number(0x02, 1), 1},  {Number(0x02, 2), 1},  {Number(0x02, 3), 1},  {Number(0x02, 4), 1},
    {Number(0x05, 3), 5},  {Number(0x0E, 2), 10}, {Number(0x14, 3), 5},  {Number(0x19, 2), 2},  {Number(0x1B, 3), 2},
    {Number(0x1C, 2), 10}, {Number(0x1E, 3), 5},  {Number(0x24, 2), 1},  {Number(0x25, 2), 2},  {Number(0x28, 1), 2},
    {Number(0x28, 3), 2},  {Number(0x2A, 2), 2},  {Number(0x2C, 2), 10}, {Number(0x37, 2), 5},  {Number(0x39, 2), 10},
    {Number(0x3F, 3), 1},  {Number(0x48, 2), 1},  {Number(0x49, 2), 1},  {Number(0x50, 2), 10}, {Number(0x56, 2), 10},
    {Number(0x57, 2), 5},  {Number(0x5A, 2), 5},  {Number(0x5B, 3), 1},  {Number(0x60, 2), 1},  {Number(0x66, 3), 5},
    {Number(0x67, 2), 5},  {Number(0x6D, 3), 5},  {Number(0x6E, 2), 10}, {Number(0x70, 2), 10}, {Number(0x79, 1), 10},
    {Number(0x7A, 2), 10}, {Number(0x7D, 2), 1},  {Number(0x7F, 2), 10}, {Number(0x80, 2), 1},  {Number(0x83, 2), 1},
    {Number(0x91, 2), 5},  {Number(0x96, 3), 10},
};

// One more group of tracks for a song that the game plays after more than one effect, in many areas each. The group has
// a track for each of the song's first `versions` versions. Each track starts with `effect`.
struct EffectGroup
{
    uint32_t song;
    uint32_t versions;
    uint32_t effect;
};

// The encounter code at 0x8004166C to 0x800416BC starts versions 1 to 3 of Normal Battle, by who strikes first.
// Effect 5 is the echo, and effect 2 the big room. The groups after those two come from Raphael Raven's states:
// - Raphael Raven follows Yoshi's Village (song 0x26, with effect 1) when the player comes from the village while
//   Raphael waits for Mario. It follows the file select's song (0x69, with effect 3, the chorus) when a save in the
//   tree loads.
// - After a battle, the function at 0x8014AE6C restores the field song and requests the effects that the song has
//   set. A song that has set none requests effect 0, no effect. Raphael Raven then plays on with no effect after a
//   battle outside the tree, in the map whose script at 0xB85C3C starts the song. The next battle there starts Normal
//   Battle with no effect.
// - Raphael Raven keeps playing outside the tree while Raphael waits for Mario. A battle there after a save in the
//   tree loads starts Normal Battle with effect 3.
constexpr uint32_t kNoEffect = 0;
constexpr EffectGroup kEffectGroups[] = {{0x02, 3, 5}, {0x02, 3, 2}, {0x02, 3, kNoEffect}, {0x02, 3, 3},
                                         {0x28, 1, 1}, {0x28, 1, 3}, {0x28, 1, kNoEffect}};

// The number of an effect group's track contains the effect above the change, or kNoEffectNumber for no effect. The
// driver's calls ignore it. It keeps the numbers of a song's tracks distinct.
constexpr int kGroupShift = 24;
constexpr uint32_t kNoEffectNumber = 0x80;

// The second version of this song lists the same phrases as the first.
constexpr uint32_t kRepeatedVersion = Number(0x61, 2);

// The end of RDRAM. A run of the game leaves zeros from 0x8038A978 to the end.
constexpr uint32_t kDriverAddress = 0x803FF000;

// A song in the game's list, with the number of versions.
struct Song
{
    uint32_t number;
    uint32_t versions;
};

// The songs in the game's list. Many entries in the list repeat Goomba Village, and the set has it once.
constexpr Song kSongs[] = {
    {0x00, 2}, {0x01, 2}, {0x02, 4}, {0x03, 3}, {0x04, 2}, {0x05, 3}, {0x07, 2}, {0x08, 2}, {0x09, 2}, {0x0A, 2},
    {0x0B, 3}, {0x0C, 2}, {0x0D, 3}, {0x0E, 2}, {0x0F, 2}, {0x11, 1}, {0x12, 2}, {0x13, 2}, {0x14, 3}, {0x15, 2},
    {0x16, 1}, {0x17, 1}, {0x18, 2}, {0x19, 2}, {0x1A, 2}, {0x1B, 3}, {0x1C, 2}, {0x1D, 1}, {0x1E, 3}, {0x1F, 2},
    {0x20, 1}, {0x21, 1}, {0x22, 1}, {0x24, 2}, {0x25, 2}, {0x26, 3}, {0x27, 2}, {0x28, 3}, {0x29, 3}, {0x2A, 2},
    {0x2B, 4}, {0x2C, 2}, {0x2D, 1}, {0x2E, 1}, {0x2F, 1}, {0x30, 2}, {0x31, 2}, {0x32, 1}, {0x33, 2}, {0x34, 2},
    {0x35, 2}, {0x37, 2}, {0x38, 1}, {0x39, 2}, {0x3A, 1}, {0x3B, 1}, {0x3C, 1}, {0x3D, 1}, {0x3E, 1}, {0x3F, 3},
    {0x40, 2}, {0x41, 3}, {0x42, 2}, {0x44, 1}, {0x45, 1}, {0x46, 2}, {0x47, 3}, {0x48, 2}, {0x49, 2}, {0x4A, 2},
    {0x4B, 2}, {0x4C, 2}, {0x4D, 1}, {0x4E, 2}, {0x4F, 1}, {0x50, 2}, {0x51, 1}, {0x52, 4}, {0x53, 1}, {0x54, 2},
    {0x55, 2}, {0x56, 2}, {0x57, 2}, {0x58, 2}, {0x59, 1}, {0x5A, 2}, {0x5B, 3}, {0x5C, 2}, {0x5D, 2}, {0x5E, 1},
    {0x5F, 1}, {0x60, 2}, {0x61, 3}, {0x62, 3}, {0x63, 3}, {0x64, 3}, {0x65, 2}, {0x66, 3}, {0x67, 2}, {0x68, 2},
    {0x69, 2}, {0x6A, 3}, {0x6B, 3}, {0x6C, 1}, {0x6D, 3}, {0x6E, 2}, {0x6F, 1}, {0x70, 3}, {0x71, 1}, {0x72, 1},
    {0x73, 2}, {0x74, 2}, {0x75, 2}, {0x76, 1}, {0x77, 3}, {0x78, 2}, {0x79, 3}, {0x7A, 2}, {0x7B, 3}, {0x7C, 3},
    {0x7D, 2}, {0x7E, 3}, {0x7F, 2}, {0x80, 2}, {0x81, 2}, {0x82, 2}, {0x83, 2}, {0x84, 1}, {0x85, 1}, {0x86, 2},
    {0x87, 1}, {0x88, 1}, {0x89, 4}, {0x8A, 1}, {0x8B, 2}, {0x8C, 2}, {0x8D, 2}, {0x8E, 2}, {0x90, 2}, {0x91, 2},
    {0x94, 1}, {0x95, 4}, {0x96, 3},
};

// The request word that starts a track with `effect`.
RdramPatch EffectPatch(uint32_t effect)
{
    return {.address = kEffectRequest, .words = {(effect << kEffectShift) | kEffectRequested}};
}

// Adds a track for each version of `song`, with the effect that the version starts with.
void AddVersionTracks(const Song& song, std::vector<Track>& tracks)
{
    for (uint32_t version = 1; version <= song.versions; version++)
    {
        Track track;
        track.number = Number(song.number, version);
        if (track.number == kRepeatedVersion)
        {
            continue;
        }

        track.name = NumberTitle("Song", song.number, TitleBase::kHex);

        for (const StartEffect& effect : kStartEffects)
        {
            if (effect.number == track.number)
            {
                track.rdram_patches.push_back(EffectPatch(effect.effect));
            }
        }

        if (track.number == kShortTrack)
        {
            track.length_seconds = kShortTrackSeconds;
        }
        tracks.push_back(track);
    }
}

// Adds a track for each version of each effect group of `song`.
void AddEffectTracks(const Song& song, std::vector<Track>& tracks)
{
    for (const EffectGroup& group : kEffectGroups)
    {
        if (group.song != song.number)
        {
            continue;
        }

        for (uint32_t version = 1; version <= group.versions; version++)
        {
            Track track;
            const uint32_t group_number = group.effect == kNoEffect ? kNoEffectNumber : group.effect;
            track.number = (group_number << kGroupShift) | Number(song.number, version);
            track.name = NumberTitle("Song", song.number, TitleBase::kHex);
            track.rdram_patches.push_back(EffectPatch(group.effect));
            tracks.push_back(track);
        }
    }
}

// Adds a track for each change of `song`.
void AddChangeTracks(const Song& song, std::vector<Track>& tracks)
{
    for (const Change& change : kChanges)
    {
        if (change.song == song.number)
        {
            Track track;
            track.number = (change.change << kChangeShift) | song.number;
            track.name = NumberTitle("Song", song.number, TitleBase::kHex);
            tracks.push_back(track);
        }
    }
}

// The bytes of instruction words in the ROM's byte order.
std::vector<uint8_t> RomBytes(std::initializer_list<uint32_t> words)
{
    std::vector<uint8_t> bytes(4 * words.size());
    size_t offset = 0;
    for (const uint32_t word : words)
    {
        xsflib::StoreBe32(bytes.data() + offset, word);
        offset += 4;
    }

    return bytes;
}

} // namespace

GameProfile NusNmqeUsa()
{
    GameProfile profile;
    profile.game = "Paper Mario";
    profile.serial = "NUS-NMQE-USA";
    profile.sha1 = "3837f44cda784b466c9a2d99df70d77c322b97a0";
    profile.cic = ultralib::machine::Cic::k6103;
    profile.sound_driver = "Paper Mario BGM player";
    profile.controller = true;

    // A return in the function that writes to cartridge space, and the checksums of the ROM with it.
    profile.rom_patches.push_back({.offset = 0x10, .bytes = {0xBD, 0xE3, 0x77, 0x9B, 0x94, 0x92, 0x26, 0xEA}});
    profile.rom_patches.push_back({.offset = 0x1060, .bytes = RomBytes({mips::Jr(mips::kRa), mips::kNop})});

    profile.snapshot_address = kFirstSongSetup;
    profile.snapshot_hit = 1;
    profile.quiet_tries = 1;
    profile.boot_seconds = 3.0;

    CallDriverOptions driver;
    driver.address = kDriverAddress;
    driver.start_calls = {
        {.function = kLoadSong, .arguments = {{.track = true, .mask = kSongMask}, {.value = kMusicPlayer}}},
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

    // Between frames the driver waits for retrace messages, as the game thread does. The threads below it run
    // meanwhile.
    driver.wait_call = MessageWait(kRecvMesg, kRetraceQueue);
    driver.fields = kFieldsPerFrame;
    profile.driver = BuildCallDriver(driver);

    profile.loop_watches.push_back(
        {.address = kLoopFirstPass, .target_register = mips::kT0, .match_register = mips::kA1, .match_value = 0});
    profile.loop_watches.push_back(
        {.address = kLoopLaterPass, .target_register = mips::kT0, .match_register = mips::kV0, .match_value = 0});

    for (const Song& song : kSongs)
    {
        AddVersionTracks(song, profile.tracks);
        AddEffectTracks(song, profile.tracks);
        AddChangeTracks(song, profile.tracks);
    }

    return profile;
}

} // namespace usf::rip::games
