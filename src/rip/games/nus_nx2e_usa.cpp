// SPDX-License-Identifier: MIT

// Gex 64: Enter the Gecko (USA): the game's music, through an early version of Software Creations' music library. The
// game shows no titles for its songs. Each title names the levels that play the song, by the names in the game's table
// of levels at 0x8006FCE0, such as horror4 or looney30. An entry of that table has 32 bytes, and its name starts at
// byte 20.
//
// Addresses come from the game's code in the image and from runs in usf's core. The main segment is copied from
// cartridge offset 0x1000 to 0x80000400.
//
// - Each song is a file of the library's early form, compressed with DEFLATE. The table of levels at 0x8006F5A8 holds
//   36 bytes for each of the 31 levels: the song's volume, the reverb base, the level's table of music triggers, the
//   cartridge offsets of the bank's samples and of its pointer table, the songs, and a flag. A level has one song, or a
//   list of up to 7 at a pointer: the cartridge offset and the volume of each, with the ends in a second list.
// - The function at 0x800327C0 takes a level's number. It loads the level's bank, unpacks its songs into slots, and
//   starts slot 0 at the song's volume, unless the level's flag is set. It also loads the bank of short songs,
//   EXTRAS.WBK. The game calls it when a level starts.
// - The game thread calls the music update at 0x80032BD0 with the level once a frame, at 30 frames a second. The update
//   restarts the playing slot's song when it ends, fades from one slot to another, and starts a short song.
// - The function at 0x80050A80 sets one of the level's script variables to a value. It looks the pair up in the level's
//   table of triggers, and asks the update to fade to the slot that the entry names. The game's code and the levels'
//   objects make the calls.
// - The function at 0x8003335C takes the number of a short song, 1 to 4. It loads the song from the table at 0x8006FA0C
//   (start, end and volume of each), and the update starts it two frames later. The level's song stops for it.
// - The game's settings give the songs a master volume of 0x3FFF at 0x800C5714. The options menu changes it in steps
//   of 0x400, up to 0x7FFF. The tracks keep the default. The synthesizer squares a voice's volume at 0x80060494. The
//   menu's maximum therefore plays 12 dB louder than the default.
// - The library gives each song's channels the level's volume (0x80054164) and reverb base (0x80054330). A channel's
//   effect level is then base + (128 - base) * reverb / 128.
// - Two songs are never played. Slot 2 of looney69, at cartridge offset 0x459FF0, has no trigger in the level's table.
//   The song at cartridge offset 0x47B670 is in no table, between the songs of the bank prehismx.wbk and the next bank.
//   Their tracks play them through the level's table, in place of the level's first song.
// - The snapshot takes over the game thread at its first call of 0x800327C0, for the level logo3 at boot. Every track
//   sets the game's current level, the byte at 0x800C5761, to its level.
// - At each retrace the scheduler sends a message to its clients, at 0x8005C734, whether the game draws or not. The
//   game thread (0x800AB900, priority 10) waits for its messages at 0x8003B840, on the queue at 0x800E9748, through
//   osRecvMesg at 0x800575C0. The queue holds 16 messages. It is empty at the snapshot, but the start calls take many
//   fields, unpacking the songs, and the messages of those fields wait in it. The audio thread (0x800CF598, priority 14) has a
//   higher priority. Of the threads with a lower one, only the idle thread (0x800ABAB0, priority 0) gets messages. The
//   idle loop at 0x8003B7E8 is a jump to itself.
// - Each channel of a song loops separately. At 0x80053584 the library jumps back to a loop's start, with the channel
//   in a0, the loop's depth in a2 and its count in v1. A count of 0xFF repeats for ever.

#include "rip/driver.h"
#include "rip/games/games.h"
#include "rip/games/tracks.h"
#include "rip/mips.h"

namespace usf::rip::games
{
namespace
{

constexpr uint32_t kLoadLevelMusic = 0x800327C0;
constexpr uint32_t kMusicUpdate = 0x80032BD0;
constexpr uint32_t kSetScriptVariable = 0x80050A80;
constexpr uint32_t kStartShortSong = 0x8003335C;
constexpr uint32_t kLoopJump = 0x80053584;
constexpr uint32_t kRecvMesg = 0x800575C0;
constexpr uint32_t kRetraceQueue = 0x800E9748;
constexpr int kQueueMessages = 16;

// The word that holds the current level in its second byte.
constexpr uint32_t kCurrentLevelWord = 0x800C5760;
constexpr int kCurrentLevelShift = 16;

// The table of levels, and the fields of an entry.
constexpr uint32_t kLevelTable = 0x8006F5A8;
constexpr uint32_t kLevelEntrySize = 36;
constexpr uint32_t kLevelVolume = 0x00;
constexpr uint32_t kLevelSongStart = 0x18;
constexpr uint32_t kLevelFlags = 0x20;

// The end of RDRAM. A run of the game holds zeros there.
constexpr uint32_t kDriverAddress = 0x803FF000;

// A track's number holds the level in its low byte. A trigger's variable and value follow in the next two bytes, or the
// number of a short song in the third byte. The top byte says how the track starts. A song that replaces a level's song
// starts as the level's song does, and its number tells it apart in the second byte.
constexpr uint32_t kLevelMask = 0xFF;
constexpr int kVariableShift = 8;
constexpr int kValueShift = 16;
constexpr uint32_t kTrigger = 0x01000000;
constexpr uint32_t kShortSong = 0x02000000;
constexpr uint32_t kReplaced = 0x04000000;

// The update starts a short song two frames after the call. Later updates would restart the level's first song at the
// volume of a slot that never faded in, which is 0.
constexpr uint32_t kShortSongFrames = 4;

// Levels by their numbers in the table.
constexpr uint32_t kLooney30 = 0;
constexpr uint32_t kHorror4 = 1;
constexpr uint32_t kRta1 = 2;
constexpr uint32_t kCircuit5 = 4;
constexpr uint32_t kKungfu02 = 5;
constexpr uint32_t kScifi10 = 6;
constexpr uint32_t kPrehst1 = 7;
constexpr uint32_t kLooney69 = 8;
constexpr uint32_t kRezop3 = 13;
constexpr uint32_t kAztec16 = 14;
constexpr uint32_t kNypd01 = 16;
constexpr uint32_t kSpy2 = 17;
constexpr uint32_t kGillig1 = 21;
constexpr uint32_t kGexzil9 = 23;
constexpr uint32_t kFinal01 = 24;
constexpr uint32_t kLogo3 = 28;

// A song that starts with its level.
struct LevelSong
{
    uint32_t level;
    const char* name;
};

constexpr LevelSong kLevelSongs[] = {
    {kHorror4, "horror4, horror2, horror6 and rta1"},
    {kCircuit5, "circuit5, circuit9, circuit0 and mooshu1"},
    {kKungfu02, "kungfu02, kungfu1 and kungfu4"},
    {kScifi10, "scifi10 and scifi14"},
    {kPrehst1, "prehst1 and prehst2"},
    {kRezop3, "rezop3 and rezop2"},
    {kAztec16, "aztec16"},
    {kNypd01, "nypd01"},
    {kSpy2, "spy2 and map5"},
    {kGillig1, "gillig1"},
    {kGexzil9, "gexzil9"},
    {kFinal01, "final01"},
};

// A song that a level's trigger starts: the first entry of the level's table that names its slot. The level mooshu1
// has the songs of circuit5 and the empty table of triggers at 0x8006F3C0. It never plays the second song.
struct TriggerSong
{
    uint32_t level;
    uint32_t variable;
    uint32_t value;
    const char* name;
};

constexpr TriggerSong kTriggerSongs[] = {
    {kRta1, 0, 2, "rta1, second song"},
    {kCircuit5, 0, 3, "circuit5, circuit9 and circuit0, second song"},
    {kLooney30, 2, 3, "looney30 and looney69, song 1"},
    {kLooney30, 0, 0, "looney30 and looney69, song 2"},
    {kLooney30, 0, 1, "looney30 and looney69, song 3"},
    {kLooney30, 2, 1, "looney30, song 4"},
    {kLooney30, 2, 2, "looney30 and looney69, song 5"},
    {kLooney69, 0, 2, "looney69, song 6"},
    {kLooney69, 0, 6, "looney69, song 7"},
};

// A song that the game never plays. Its track puts it in place of a level's single song.
struct UnusedSong
{
    uint32_t level;
    uint32_t start;
    uint32_t end;
    uint32_t volume;
    const char* name;
};

constexpr UnusedSong kUnusedSongs[] = {
    {kLooney69, 0x459FF0, 0x45A5C0, 0xA0, "looney69, song 8, never played"},
    {kPrehst1, 0x47B670, 0x47CCB0, 0x60, "prehismx.wbk, second song, never played"},
};

constexpr int kShortSongs = 4;

uint32_t LevelEntry(uint32_t level, uint32_t field)
{
    return kLevelTable + level * kLevelEntrySize + field;
}

RdramPatch CurrentLevel(uint32_t level)
{
    return {.address = kCurrentLevelWord, .words = {level << kCurrentLevelShift}};
}

} // namespace

GameProfile NusNx2eUsa()
{
    GameProfile profile;
    profile.game = "Gex 64: Enter the Gecko";
    profile.serial = "NUS-NX2E-USA";
    profile.sha1 = "16042cd0dfa5439ca436f1bf05ebfb7e9f730cda";
    profile.year = "1998";
    profile.sound_driver = kSoftwareCreationsLibrary;

    profile.snapshot_address = kLoadLevelMusic;
    profile.snapshot_hit = 1;
    profile.quiet_tries = 1;
    profile.boot_seconds = 5.0;

    const CallArgument level = {.track = true, .mask = kLevelMask};

    CallDriverOptions driver;
    driver.address = kDriverAddress;
    driver.start_calls = {
        {.function = kLoadLevelMusic, .arguments = {level}},
        {.function = kSetScriptVariable,
         .arguments = {{.track = true, .right_shift = kVariableShift, .mask = 0xFF},
                       {.track = true, .right_shift = kValueShift, .mask = 0xFF}},
         .track_mask = kTrigger,
         .track_value = kTrigger},
        {.function = kStartShortSong,
         .arguments = {{.track = true, .right_shift = kValueShift, .mask = 0xFF}, {.value = 0}},
         .track_mask = kShortSong,
         .track_value = kShortSong},
    };
    driver.frame_calls = {
        {.function = kMusicUpdate, .arguments = {level}, .track_mask = kShortSong, .track_value = 0},
        {.function = kMusicUpdate,
         .arguments = {level},
         .track_mask = kShortSong,
         .track_value = kShortSong,
         .frames = kShortSongFrames},
    };
    // The driver drops the messages that came during the start calls. Between frames it waits for two retrace
    // messages, as the game thread does. The idle thread runs meanwhile.
    for (int i = 0; i < kQueueMessages; i++)
    {
        driver.start_calls.push_back(MessageDrop(kRecvMesg, kRetraceQueue));
    }
    driver.wait_call = MessageWait(kRecvMesg, kRetraceQueue);
    driver.fields = 2;
    profile.driver = BuildCallDriver(driver);

    profile.loop_watches.push_back(
        {.address = kLoopJump, .target_register = mips::kA2, .track_register = mips::kA0, .count_register = mips::kV1});

    for (const LevelSong& song : kLevelSongs)
    {
        Track track = NumberedTrack(song.level, song.name);
        track.rdram_patches = {CurrentLevel(song.level)};
        profile.tracks.push_back(track);
    }

    for (const TriggerSong& song : kTriggerSongs)
    {
        Track track = NumberedTrack(kTrigger | song.value << kValueShift | song.variable << kVariableShift | song.level,
                                    song.name);
        // The level's first song waits for a trigger, as the songs of looney30 and looney69 do.
        track.rdram_patches = {CurrentLevel(song.level),
                               {.address = LevelEntry(song.level, kLevelFlags), .words = {1}}};
        profile.tracks.push_back(track);
    }

    uint32_t unused = 0;
    for (const UnusedSong& song : kUnusedSongs)
    {
        Track track = NumberedTrack(kReplaced | unused++ << kVariableShift | song.level, song.name);
        track.rdram_patches = {
            CurrentLevel(song.level),
            {.address = LevelEntry(song.level, kLevelVolume), .words = {song.volume}},
            {.address = LevelEntry(song.level, kLevelSongStart), .words = {song.start, song.end}},
            {.address = LevelEntry(song.level, kLevelFlags), .words = {0}},
        };
        profile.tracks.push_back(track);
    }

    for (int number = 1; number <= kShortSongs; number++)
    {
        Track track =
            NumberedTrack(kShortSong | static_cast<uint32_t>(number) << kValueShift | kLogo3,
                          NumberTitle("EXTRAS.WBK, short song", static_cast<uint32_t>(number), TitleBase::kDecimal));
        track.rdram_patches = {CurrentLevel(kLogo3)};
        profile.tracks.push_back(track);
    }

    return profile;
}

} // namespace usf::rip::games
