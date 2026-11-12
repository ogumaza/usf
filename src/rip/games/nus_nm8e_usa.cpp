// SPDX-License-Identifier: MIT

// Mario Tennis (USA): the game's music, through Software Creations' music library. The game has no sound test. Each
// title is the name of the song in the game's song table.
//
// Addresses come from the game's code in the image and from runs in usf's core.
//
// - The game's code at 0x802EC000 is an overlay that it unpacks at boot. The table at 0x802F4ABC holds 66 songs: the
//   file number of each one in the game's file system and its name.
// - The function at 0x802ECD54 takes a song's place in that table and asks for the song. The game thread calls the
//   function at 0x802ECC60 once a frame. That function then loads the file, starts the song and gives its handle the
//   game's level.
// - The snapshot takes over the game thread at the first call of the function at 0x802ECD54.
// - The trophy scene in the game's code at 0x800E7730 sets the tempo of three songs just after it starts them:
//   TRO-MARIO, TRO-WALUI, and TRO-BABYM for character 0x11. Character 6 plays TRO-BABYM at its normal tempo. The
//   function at 0x802ECBCC takes the scale in a1.
// - Each channel of a song loops separately. At 0x802F1254 the library has reached the end of a loop, with its count in
//   v1, the channel in a0 and the loop's depth in a2. A count of 0xFF repeats for ever.

#include <iterator>

#include "rip/driver.h"
#include "rip/games/games.h"
#include "rip/games/tracks.h"
#include "rip/mips.h"

namespace usf::rip::games
{
namespace
{

constexpr uint32_t kSelectSong = 0x802ECD54;
constexpr uint32_t kFrameSound = 0x802ECC60;
constexpr uint32_t kLoopEnd = 0x802F1254;
constexpr uint32_t kSetTempo = 0x802ECBCC;

// A track's number holds the song's place in the table, and a second version above it.
constexpr uint16_t kPlaceMask = 0xFF;
constexpr int kVersionShift = 8;
constexpr uint32_t kAllMask = 0xFFFFFFFF;
constexpr uint32_t kTroBabyM = 0x2A;
constexpr uint32_t kTroMario = 0x2F;
constexpr uint32_t kTroWalui = 0x3A;
constexpr uint32_t kFasterTroBabyM = (1 << kVersionShift) | kTroBabyM;

struct Tempo
{
    uint32_t number;
    uint32_t scale;
};

constexpr Tempo kTempos[] = {{kFasterTroBabyM, 0x8E}, {kTroMario, 0x7C}, {kTroWalui, 0x78}};

// The game starts a song two frames after asking for it, and sets the tempo at its fourth frame. The driver makes the
// call in each of the first frames. A call before the song starts reaches no channel.
constexpr uint32_t kTempoFrames = 6;

// The end of RDRAM. A run of the game holds zeros there.
constexpr uint32_t kDriverAddress = 0x803FF000;

constexpr const char* kSongs[] = {
    "BG01",      "BG02",       "BG03",      "BG04",      "BG05",      "BG08",        "BG09",      "BG10",
    "BG11",      "VS",         "BGM",       "BGM2",      "MAKE",      "FINAL",       "KACHI",     "MENU",
    "RESULT",    "GAMEPT",     "KACHI2",    "MAKE2",     "MAKE3",     "MARIO",       "MARIO2",    "MARIO4",
    "PEACH",     "PEACH2",     "PEACH3",    "SETPOINT",  "VS2",       "TAIBREAK",    "END1",      "END2",
    "COMIKAL",   "E_JINGLE",   "KUPPA",     "TITLE",     "DONKY",     "WARIO",       "YOSSI",     "DOUBLES",
    "MUTEKI",    "SINGLE",     "TRO-BABYM", "TRO-DKJR",  "TRO-DONKY", "TRO-KINOPIO", "TRO-KUPPA", "TRO-MARIO",
    "TRO-PEACH", "TRO-WARIO",  "TRO-YOSSI", "TRO-CASAR", "TRO-DAISY", "TRO-GB",      "TRO-HEIHO", "TRO-LUISI",
    "TRO-PATA",  "TRO-TERESA", "TRO-WALUI", "TRO-BOY",   "TRO-GIRL",  "TRO-MASSOU",  "OP1",       "OP2",
    "OP3",       "OP4",
};

// The channels of a song loop separately, and the rip's length model doesn't settle on one length. Each entry is the
// time that the slowest channel takes to reach its loop's second pass, measured in usf's core. A song that ends by
// itself has 0, and the rip finds where its sound stops.
constexpr double kSongSeconds[] = {
    185.4, 308.6, 256.2, 224.2, 184.1, 190.3, 166.6, 185.5, 169.4, 40.3,  232.3, 257.2, 28.3,  227.2,
    27.4,  102.4, 71.3,  76.8,  0.0,   0.0,   0.0,   207.7, 230.3, 207.7, 114.6, 134.7, 114.6, 115.2,
    41.7,  191.8, 298.6, 336.7, 22.9,  0.0,   185.2, 0.0,   190.4, 104.2, 157.7, 0.0,   6.0,   0.0,
    0.0,   0.0,   0.0,   0.0,   0.0,   0.0,   0.0,   0.0,   0.0,   0.0,   0.0,   0.0,   0.0,   0.0,
    0.0,   0.0,   0.0,   0.0,   0.0,   0.0,   0.0,   0.0,   0.0,   0.0,
};

static_assert(std::size(kSongSeconds) == std::size(kSongs));

} // namespace

GameProfile NusNm8eUsa()
{
    GameProfile profile;
    profile.game = "Mario Tennis";
    profile.serial = "NUS-NM8E-USA";
    profile.sha1 = "999047f07cec931ffbdcc7b33b8502ef602807ee";
    profile.year = "2000";
    profile.sound_driver = kSoftwareCreationsLibrary;
    profile.controller = true;

    profile.snapshot_address = kSelectSong;
    profile.snapshot_hit = 1;
    profile.quiet_tries = 1;
    profile.boot_seconds = 8.0;

    CallDriverOptions driver;
    driver.address = kDriverAddress;
    driver.start_calls = {{.function = kSelectSong, .arguments = {{.track = true, .mask = kPlaceMask}}}};
    driver.frame_calls = {{.function = kFrameSound}};
    for (const Tempo& tempo : kTempos)
    {
        GameCall call;
        call.function = kSetTempo;
        // The function changes the tempo of the song that plays. It doesn't read a0.
        call.arguments = {{.value = 0}, {.value = tempo.scale}};
        call.track_mask = kAllMask;
        call.track_value = tempo.number;
        call.frames = kTempoFrames;
        driver.frame_calls.push_back(call);
    }

    driver.fields = 2;
    profile.driver = BuildCallDriver(driver);

    profile.loop_watches.push_back(
        {.address = kLoopEnd, .target_register = mips::kA2, .track_register = mips::kA0, .count_register = mips::kV1});

    uint32_t number = 0;
    for (const char* name : kSongs)
    {
        Track track;
        track.number = number++;
        track.name = name;
        if (kSongSeconds[track.number] > 0.0)
        {
            track.length_seconds = kSongSeconds[track.number];
            track.fade_seconds = kMeasuredFadeSeconds;
        }
        profile.tracks.push_back(track);

        if (track.number == kTroBabyM)
        {
            track.number = kFasterTroBabyM;
            track.name = "TRO-BABYM, faster";
            profile.tracks.push_back(track);
        }
    }

    return profile;
}

} // namespace usf::rip::games
