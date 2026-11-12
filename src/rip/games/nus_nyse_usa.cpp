// SPDX-License-Identifier: MIT

// Yoshi's Story (USA): the game's music, played through Nintendo EAD's sound engine in an early form of the build that
// Ocarina of Time uses. Its tables of sequences, banks and samples have the later engine's layout.
//
// Addresses come from the game's code in the image and from runs in usf's core, with a 16 Kbit EEPROM connected.
// Without the EEPROM the game stops after 4 s of its boot. The main segment is copied from cartridge offset 0x1000 to
// 0x80000400.
//
// - The audio set-up at 0x80007D88 reads the sequence table at 0x800B4860 (65 entries), the bank table at 0x800B4360
//   and the sample table at 0x800B4C80. The sequences start at cartridge offset 0x4F3930. Sequence 0 contains the sound
//   effects. Entries 62 and 63 point to sequence 1.
// - The game's music layer keeps a music number. The function at 0x80010730 takes the number in a0 and turns it into a
//   sequence through the switch at 0x800116C4: 0x01 to 0x44, of which 0x0C and 0x0D give sequences 0x02 and 0x08 again,
//   0x40 and 0x41 give entries 62 and 63, and 0x43 and 0x44 give sequences past the table's end. It then starts the
//   sequence through 0x80011600 on the main music handle at 0x80099290, with a fade of 0x78 frames.
// - 0x80011600 applies the mood at 0x800992D0 through 0x800186E0. A mood mutes channels, and for some sequences changes
//   the tempo and the pitch of some channels. Moods 0 and 1 mute the same channels: 11 to 14 for sequences 0x01 to
//   0x0A, 0x11, 0x14, 0x23, 0x2D and 0x32, and 14 for the others. The code at 0x8002B3A0 sets the mood from the current
//   Yoshi's health: 0 for 1 to 7, 1 for 8, 2 for 0 and 3 for more than 8. 0x80010730 sets mood 1 in place of mood 3
//   before it starts a sequence. Mood 3 therefore comes only while a sequence plays.
// - Mood 2 sets a tempo change of -60 and, for the channels of a table, a frequency of 0.67, both reached over 60
//   frames. Mood 3 mutes channels 0 to 10 and 14 of the sequences listed above. Channels 11 to 13 then play.
// - The music handle's fields go to the audio thread through requests that 0x80019184 sends each frame: the sequence's
//   start (command 0x82), the muted channels (commands 0x90 and 0x08), the tempo change (0x49), and the channels'
//   frequency (0x04).
// - The sound frame at 0x80010558 runs the music layer and sends its commands. The game loop at 0x80068E24 calls it
//   once a frame, every second video field.
// - The script reader's flow control at 0x8000D398 handles a sequence's absolute jumps (commands 0xF5 and 0xF9 to 0xFB)
//   at 0x8000D50C to 0x8000D51C. At 0x8000D518 the new script address is in t9, the script's state in a1 and the
//   sequence player in a0. Music plays on sequence player 0, at 0x800B7740. The state of its script is at 0x800B77B4.
// - The sound set-up starts sequence 0, the sound effects, on sequence player 2. At the snapshot, the requests for the
//   jingle of the boot's logo wait in the music layer's queue. The music layer sends them once player 2 runs.
//   0x80010730 restarts player 2. The driver then stops it through 0x80019D78. The jingle never plays. The music plays
//   on player 0.
// - For music 0x24, 0x80010730 sets the muted channels from the word at 0x800992E8. Each of channels 0 to 7 plays only
//   while the flag of one Yoshi is set. All eight flags start set in the image's data. They're still set at the
//   snapshot, and every channel plays.
// - Music 0x0F has six phases. Each mutes a different set of channels. The function at 0x80018B78 sets the phase, and
//   the stage's code changes it during play. 0x80011600 starts phase 2. The tracks of the other phases set them after
//   the music starts.
// - For music 0x32, 0x80010730 mutes channel 3, channels 2 and 3, or channels 1 to 3 when the halfword at 0x800992F0 is
//   2, 3 or 4. It's 1 at the snapshot. Then 0x80010730 mutes no more channels. The other states only take channels
//   away. The rip has no track for them.

#include <cstdio>
#include <iterator>
#include <string>
#include <vector>

#include "rip/driver.h"
#include "rip/games/games.h"
#include "rip/mips.h"

namespace usf::rip::games
{
namespace
{

constexpr uint32_t kSetMusic = 0x80010730;
constexpr uint32_t kSetMood = 0x800186E0;
constexpr uint32_t kSoundFrame = 0x80010558;
constexpr uint32_t kStopSequence = 0x80019D78;
constexpr uint32_t kSetPhase = 0x80018B78;
constexpr uint32_t kSequenceJump = 0x8000D518;
constexpr uint32_t kMusicScript = 0x800B77B4;

// RDRAM that runs of the game hold at zero, from 0x8022D200 to 0x8031E000.
constexpr uint32_t kDriverAddress = 0x8031D000;

// A track's number contains the music number in bits 0 to 7, the mood in bits 8 to 11 and the phase of music 0x0F in
// bits 12 to 15.
constexpr uint16_t kMusicMask = 0xFF;
constexpr int kMoodShift = 8;
constexpr uint32_t kMoodMask = 0xF00;
constexpr int kPhaseShift = 12;
constexpr uint32_t kPhaseMask = 0xF000;

constexpr int kPhaseMusic = 0x0F;

// The phases of music 0x0F that the tracks set. 0x80011600 starts phase 2.
constexpr int kOtherPhases[] = {1, 3, 4, 5, 6};

// The moods that the tracks set: 2 for a health of 0, and 3 for a health above 8.
constexpr int kNoPetalsMood = 2;
constexpr int kHighHealthMood = 3;

// The sequence player of the sound effects.
constexpr uint32_t kEffectsPlayer = 2;

// The sequences whose music the moods change, with their music numbers.
struct MoodSong
{
    int music;
    int sequence;
};

constexpr MoodSong kMoodSongs[] = {
    {0x01, 0x01}, {0x02, 0x02}, {0x03, 0x03}, {0x04, 0x04}, {0x05, 0x05}, {0x06, 0x06}, {0x07, 0x07},
    {0x08, 0x08}, {0x09, 0x09}, {0x0A, 0x0A}, {0x0B, 0x11}, {0x0E, 0x23}, {0x0F, 0x24}, {0x10, 0x2D},
    {0x11, 0x14}, {0x2C, 0x0E}, {0x2F, 0x25}, {0x30, 0x35}, {0x36, 0x32},
};

// The sequence of each music number from 0x01 to 0x44, from the switch at 0x800116C4.
constexpr int kSequenceOfMusic[0x45] = {
    -1,   0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x11, 0x02, 0x08, 0x23, 0x24, 0x2D, 0x14,
    0x1F, 0x20, 0x21, 0x22, 0x0B, 0x10, 0x12, 0x16, 0x19, 0x17, 0x13, 0x18, 0x1A, 0x1B, 0x1C, 0x1D, 0x0F, 0x2A,
    0x0C, 0x15, 0x0D, 0x2E, 0x26, 0x1E, 0x27, 0x2B, 0x0E, 0x2C, 0x34, 0x25, 0x35, 0x28, 0x29, 0x2F, 0x30, 0x31,
    0x32, 0x33, 0x38, 0x36, 0x37, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 0x40, 0x41, 0x42,
};

// Whether each of kMoodSongs gives the sequence of its music in kSequenceOfMusic.
constexpr bool MoodSongsMatchMusic()
{
    for (const MoodSong& song : kMoodSongs)
    {
        if (kSequenceOfMusic[song.music] != song.sequence)
        {
            return false;
        }
    }

    return true;
}

static_assert(MoodSongsMatchMusic());

constexpr int kTitleMusic = 0x16;

bool Ripped(int music)
{
    // 0x0C and 0x0D repeat sequences 0x02 and 0x08. 0x40 and 0x41 name entries that point to sequence 1. With them,
    // sequence 1 plays channels 11 to 13 as well. No code that starts them is known. 0x43 and 0x44 name sequences that
    // the table doesn't hold.
    return music != 0x0C && music != 0x0D && music != 0x40 && music != 0x41 && music < 0x43;
}

std::string TrackName(int music, int mood, int phase)
{
    char name[64];
    std::snprintf(name, sizeof(name), "Music 0x%02X (sequence 0x%02X)", music, kSequenceOfMusic[music]);
    std::string text = name;
    if (music == kTitleMusic)
    {
        text += ": title screen";
    }

    if (phase != 0)
    {
        text += ", phase " + std::to_string(phase);
    }

    if (mood == kNoPetalsMood)
    {
        text += ", no petals";
    }
    else if (mood == kHighHealthMood)
    {
        text += ", health above 8";
    }

    return text;
}

// Adds the tracks of each music number: the music, the other phases of music 0x0F, and the moods of kMoodSongs.
void AddMusicTracks(std::vector<Track>& tracks)
{
    for (int music = 1; music < std::ssize(kSequenceOfMusic); music++)
    {
        if (!Ripped(music))
        {
            continue;
        }

        tracks.push_back({.number = static_cast<uint32_t>(music), .name = TrackName(music, 0, 0)});

        if (music == kPhaseMusic)
        {
            for (const int phase : kOtherPhases)
            {
                tracks.push_back({.number = static_cast<uint32_t>(music | (phase << kPhaseShift)),
                                  .name = TrackName(music, 0, phase)});
            }
        }

        for (const MoodSong& song : kMoodSongs)
        {
            if (song.music != music)
            {
                continue;
            }

            for (const int mood : {kNoPetalsMood, kHighHealthMood})
            {
                tracks.push_back(
                    {.number = static_cast<uint32_t>(music | (mood << kMoodShift)), .name = TrackName(music, mood, 0)});
            }
        }
    }
}

} // namespace

GameProfile NusNyseUsa()
{
    GameProfile profile;
    profile.game = "Yoshi's Story";
    profile.serial = "NUS-NYSE-USA";
    profile.sha1 = "b13072fef6c6df48c07d8822c01e5bc59036f6da";
    profile.year = "1997";
    profile.cic = n64::Cic::k6106;
    profile.sound_driver = "Nintendo EAD sound engine";
    profile.eeprom = n64::EepromType::k16Kbit;

    // The snapshot is the first quiet sound frame from the third on. That's the fourth, at 0.51 s, once the sound
    // engine has started and before any music plays. The game boots without a controller. The driver takes over the
    // game thread there. It starts the track's music number, stops sequence player 2, sets the track's mood or phase,
    // and runs the sound frame every second field, as the game loop does.
    profile.snapshot_address = kSoundFrame;
    profile.snapshot_hit = 3;
    profile.quiet_tries = 2;
    profile.boot_seconds = 2.0;

    CallDriverOptions driver;
    driver.address = kDriverAddress;
    driver.start_calls = {
        {.function = kSetMusic, .arguments = {{.track = true, .mask = kMusicMask}}},
        // The stop passes 0 in a1. So do the game's stops at 0x80018FB4 and 0x8001AB04.
        {.function = kStopSequence, .arguments = {{.value = kEffectsPlayer}, {.value = 0}}},
        {.function = kSetMood,
         .arguments = {{.value = kNoPetalsMood}},
         .track_mask = kMoodMask,
         .track_value = kNoPetalsMood << kMoodShift},
        {.function = kSetMood,
         .arguments = {{.value = kHighHealthMood}},
         .track_mask = kMoodMask,
         .track_value = kHighHealthMood << kMoodShift},
    };
    for (const int phase : kOtherPhases)
    {
        driver.start_calls.push_back({.function = kSetPhase,
                                      .arguments = {{.value = static_cast<uint32_t>(phase)}},
                                      .track_mask = kPhaseMask,
                                      .track_value = static_cast<uint32_t>(phase) << kPhaseShift});
    }

    driver.frame_calls = {{.function = kSoundFrame}};
    driver.fields = 2;
    profile.driver = BuildCallDriver(driver);

    // Only the jumps of the script of the music player count. Its channels and layers repeat short figures.
    profile.loop_watches.push_back({.address = kSequenceJump,
                                    .target_register = mips::kT9,
                                    .match_register = mips::kA1,
                                    .match_value = kMusicScript});

    AddMusicTracks(profile.tracks);

    return profile;
}

} // namespace usf::rip::games
