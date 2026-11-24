// SPDX-License-Identifier: MIT

// Gex 64: Enter the Gecko (USA): the game's music, through an early version of Software Creations' music library. The
// game shows no titles for its songs. The tracks go by their places in the set. The comments name the levels that play
// each song, by the names in the game's table of levels at 0x8006FCE0, such as horror4 or looney30. An entry of that
// table has 32 bytes, and its name starts at byte 20.
//
// Addresses come from the game's code in the image and from runs in usf's core. The main segment is copied from
// cartridge offset 0x1000 to 0x80000400.
//
// - Each song is a file of the library's early form, compressed with DEFLATE. The table of levels at 0x8006F5A8 has
//   36 bytes for each of the 31 levels: the song's volume, the reverb base, the level's table of music triggers, the
//   cartridge offsets of the bank's samples and of its pointer table, the songs, and a flag. A level has one song, or a
//   list of up to 7 at a pointer: the cartridge offset and the volume of each, with the ends in a second list.
// - The function at 0x800327C0 takes a level's number. It loads the level's bank, unpacks its songs into slots, and
//   starts slot 0 at the song's volume, unless the level's flag is set. It also loads the bank of short songs,
//   EXTRAS.WBK. The game calls it when a level starts.
// - The game thread calls the music update at 0x80032BD0 with the level once a frame, at 30 frames a second. The update
//   restarts the playing slot's song when it ends, fades from one slot to another, and starts a short song.
// - The function at 0x80050A80 sets one of the level's script variables to a value. It looks the pair up in the level's
//   table of triggers, and has the update fade to the slot that the entry names. The game's code and the levels'
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
//   osRecvMesg at 0x800575C0. The queue has room for 16 messages. It is empty at the snapshot. The messages of the
//   fields that the start calls take, unpacking the songs, wait in it: two for the second song of rta1. The game's loop
//   at 0x8003B834 takes each message in turn and drops none. The audio thread (0x800CF598, priority 14) has a higher
//   priority. Of the threads with a lower one, only the idle thread (0x800ABAB0, priority 0) gets messages. The idle
//   loop at 0x8003B7E8 is a jump to itself.
// - The library starts a song at 0x800543D4. It sets up each of the song's channels at 0x800557D4, with the default
//   tempo. The audio thread's tick at 0x80054510 runs the channels' commands. A tempo command (0x80053170) sets the
//   tempo of each channel of its song that is set up by then. A tick between the set-ups plays the song differently.
//   The channels set up after it keep the default tempo, and short song 4 plays its first notes early.
//   __osDisableInt is at 0x8005DD00, and __osRestoreInt at 0x8005DD20.
// - Each channel of a song loops separately. At 0x80053584 the library jumps back to a loop's start, with the channel
//   in a0, the loop's depth in a2 and its count in v1. A count of 0xFF repeats for ever.

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string>
#include <vector>

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
constexpr uint32_t kDisableInterrupts = 0x8005DD00;
constexpr uint32_t kRestoreInterrupts = 0x8005DD20;
constexpr int kQueueMessages = 16;

// The word that contains the current level in its second byte.
constexpr uint32_t kCurrentLevelWord = 0x800C5760;
constexpr int kCurrentLevelShift = 16;

// The table of levels, and the fields of an entry.
constexpr uint32_t kLevelTable = 0x8006F5A8;
constexpr uint32_t kLevelEntrySize = 36;
constexpr uint32_t kLevelVolume = 0x00;
constexpr uint32_t kLevelSongStart = 0x18;
constexpr uint32_t kLevelFlags = 0x20;

// The end of RDRAM. A run of the game leaves zeros there.
constexpr uint32_t kDriverAddress = 0x803FF000;

// A track's number contains the level in its low byte. A trigger's variable and value follow in the next two bytes, or
// the number of a short song in the third byte. The top byte says how the track starts. A song that replaces a level's
// song starts as the level's song does, and the second byte of its number distinguishes it.
constexpr uint32_t kLevelMask = 0xFF;
constexpr int kVariableShift = 8;
constexpr int kValueShift = 16;
constexpr uint32_t kTrigger = 0x01000000;
constexpr uint32_t kShortSong = 0x02000000;
constexpr uint32_t kReplaced = 0x04000000;

// A track mask that tests the whole number.
constexpr uint32_t kAllMask = 0xFFFFFFFF;

// The update starts a short song two frames after the call. Later updates would restart the level's first song at the
// volume of a slot that never faded in. That volume is 0.
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

// A song that starts with its level. A song of several levels plays as the first of them plays it. The table gives
// circuit5 and circuit9 a volume of 0x80, and circuit0 and mooshu1 0x60. Those two levels play the song of circuit5
// 5 dB quieter than its track.
struct LevelSong
{
    uint32_t level;
};

constexpr LevelSong kLevelSongs[] = {
    {kHorror4},  // horror4, horror2, horror6 and rta1.
    {kCircuit5}, // circuit5, circuit9, circuit0 and mooshu1.
    {kKungfu02}, // kungfu02, kungfu1 and kungfu4.
    {kScifi10},  // scifi10 and scifi14.
    {kPrehst1},  // prehst1 and prehst2.
    {kRezop3},   // rezop3 and rezop2.
    {kAztec16},  // aztec16.
    {kNypd01},   // nypd01.
    {kSpy2},     // spy2 and map5.
    {kGillig1},  // gillig1.
    {kGexzil9},  // gexzil9.
    {kFinal01},  // final01.
};

// A song that a level's trigger starts: the first entry of the level's table that names its slot. The level mooshu1
// has the songs of circuit5 and the empty table of triggers at 0x8006F3C0. It never plays the second song.
struct TriggerSong
{
    uint32_t level;
    uint32_t variable;
    uint32_t value;
};

constexpr TriggerSong kTriggerSongs[] = {
    {kRta1, 0, 2},     // rta1, second song.
    {kCircuit5, 0, 3}, // circuit5, circuit9 and circuit0, second song.
    {kLooney30, 2, 3}, // looney30 and looney69, song 1.
    {kLooney30, 0, 0}, // looney30 and looney69, song 2.
    {kLooney30, 0, 1}, // looney30 and looney69, song 3.
    {kLooney30, 2, 1}, // looney30, song 4.
    {kLooney30, 2, 2}, // looney30 and looney69, song 5.
    {kLooney69, 0, 2}, // looney69, song 6.
    {kLooney69, 0, 6}, // looney69, song 7.
};

// A song that the game never plays. Its track puts it in place of a level's single song.
struct UnusedSong
{
    uint32_t level;
    uint32_t start;
    uint32_t end;
    uint32_t volume;
};

constexpr UnusedSong kUnusedSongs[] = {
    {kLooney69, 0x459FF0, 0x45A5C0, 0xA0}, // looney69, song 8, never played.
    {kPrehst1, 0x47B670, 0x47CCB0, 0x60},  // prehismx.wbk, second song, never played.
};

// The index in kUnusedSongs of song 8 of looney69. Its track starts the song in the load.
constexpr size_t kLoadStartIndex = 0;

constexpr int kShortSongs = 4;

uint32_t LevelEntry(uint32_t level, uint32_t field)
{
    return kLevelTable + level * kLevelEntrySize + field;
}

// The title of the next track of `tracks`, by its place in the set: "Song 1" for the first.
std::string PlaceTitle(const std::vector<Track>& tracks)
{
    return NumberTitle("Song", static_cast<uint32_t>(tracks.size() + 1), TitleBase::kDecimal);
}

RdramPatch CurrentLevel(uint32_t level)
{
    return {.address = kCurrentLevelWord, .words = {level << kCurrentLevelShift}};
}

// The number of the track of the unused song at `index` of kUnusedSongs.
uint32_t UnusedTrack(size_t index)
{
    return kReplaced | static_cast<uint32_t>(index) << kVariableShift | kUnusedSongs[index].level;
}

} // namespace

GameProfile NusNx2eUsa()
{
    GameProfile profile;
    profile.game = "Gex 64: Enter the Gecko";
    profile.serial = "NUS-NX2E-USA";
    profile.sha1 = "16042cd0dfa5439ca436f1bf05ebfb7e9f730cda";
    profile.sound_driver = kSoftwareCreationsLibrary;

    profile.snapshot_address = kLoadLevelMusic;
    profile.snapshot_hit = 1;
    profile.quiet_tries = 1;
    profile.boot_seconds = 5.0;

    const CallArgument kLevelArgument = {.track = true, .mask = kLevelMask};

    // Song 8 of looney69 starts in the load, not in an update. In usf's core the audio thread's tick split that start
    // too. For its track the driver makes the load with interrupts off. The audio thread then waits until the load
    // ends. The loads of the other tracks start their songs whole with interrupts on. With a mask, the first sound of
    // the other unused song would come 33 ms sooner.
    CallDriverOptions driver;
    driver.address = kDriverAddress;
    driver.start_calls = {
        {.function = kDisableInterrupts, .track_mask = kAllMask, .track_value = UnusedTrack(kLoadStartIndex)},
        {.function = kLoadLevelMusic, .arguments = {kLevelArgument}},
        {.function = kRestoreInterrupts,
         .arguments = {{.value = 1}},
         .track_mask = kAllMask,
         .track_value = UnusedTrack(kLoadStartIndex)},
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

    // The driver makes each update with interrupts off. A song that the update starts is then set up whole. In the game
    // the audio thread's tick comes about 0.1 to 3 ms after a steady update. An update that starts a song takes 0.5 to
    // 0.8 ms. A sooner tick splits the start. Short song 4 then plays at once rather than about 0.3 s later. A split
    // start is a race of timing, not a state of the game. Every track keeps the whole start. In usf's core a channel's
    // set-up takes about twice as long as in ares. Without the mask, a tick split some starts and not others. A level's
    // song and a short song make two kinds of frame. A short song's frames end after four.
    for (const GameCall& call :
         {GameCall{.function = kDisableInterrupts}, GameCall{.function = kMusicUpdate, .arguments = {kLevelArgument}},
          GameCall{.function = kRestoreInterrupts, .arguments = {{.value = 1}}}})
    {
        GameCall level_frame = call;
        level_frame.track_mask = kShortSong;
        driver.frame_calls.push_back(level_frame);
        GameCall short_frame = level_frame;
        short_frame.track_value = kShortSong;
        short_frame.frames = kShortSongFrames;
        driver.frame_calls.push_back(short_frame);
    }

    // The driver drops the messages that came during the start calls. The game takes them. After its first 0.15 s, its
    // output is the same with them or without them. Without the drops, a short song's four frames would run back to
    // back. Between frames the driver waits for two retrace messages, as the game thread does. The idle thread runs
    // meanwhile.
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
        Track track = NumberedTrack(song.level, PlaceTitle(profile.tracks));
        track.rdram_patches = {CurrentLevel(song.level)};
        profile.tracks.push_back(track);
    }

    for (const TriggerSong& song : kTriggerSongs)
    {
        Track track = NumberedTrack(kTrigger | song.value << kValueShift | song.variable << kVariableShift | song.level,
                                    PlaceTitle(profile.tracks));
        // The level's first song waits for a trigger, as the songs of looney30 and looney69 do.
        track.rdram_patches = {CurrentLevel(song.level),
                               {.address = LevelEntry(song.level, kLevelFlags), .words = {1}}};
        profile.tracks.push_back(track);
    }

    for (size_t i = 0; i < std::size(kUnusedSongs); i++)
    {
        const UnusedSong& song = kUnusedSongs[i];
        Track track = NumberedTrack(UnusedTrack(i), PlaceTitle(profile.tracks));
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
        // A short song of the bank EXTRAS.WBK.
        Track track = NumberedTrack(kShortSong | static_cast<uint32_t>(number) << kValueShift | kLogo3,
                                    PlaceTitle(profile.tracks));
        track.rdram_patches = {CurrentLevel(kLogo3)};
        profile.tracks.push_back(track);
    }

    return profile;
}

} // namespace usf::rip::games
