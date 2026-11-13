// SPDX-License-Identifier: MIT

// The Legend of Zelda: Ocarina of Time (USA, revision 2): the game's music. The game has no sound test. Each title
// names the place or the event where the game plays the sequence. A scene's header in the image gives the sequence of
// each place.
//
// Addresses come from the game's code in the image and from runs in usf's core. The game requires the CIC-NUS-6105's
// answers, which come from a dump of the console's PIF ROM: usfrip --pif FILE.
//
// - The sequence table at 0x80114220 holds 110 sequences. Sequence 0 holds the sound effects. Sequence 0x57 points to
//   sequence 0x28, so the set leaves it out. Sequences 0x01 and 0x6D hold ambience and cutscene effects. They stay
//   silent until the game's code sets their ports, so the set leaves them out too.
// - The function at 0x800CB2B0 takes a 32-bit command in a0 and queues it for the audio update. Command 0x000000NN
//   starts sequence NN on sequence player 0. Command 0xE0000000 sets stereo output. Command 0xF00000NN resets the sound
//   engine to audio set-up NN.
// - The game thread (entry 0x800A2014) calls the frame function at 0x800A1D68 once a frame. That function ends with a
//   call of the audio update at 0x800C4490, which reads the queue. Gameplay has a frame of three video fields.
// - The snapshot takes over the game thread at the frame function's first call. Audio buffers start soon after. The
//   first calls are quiet. Later ones aren't: the audio thread keeps AI's second buffer waiting.
// - The game's interrupt manager at 0x8011E038 sends a message to each of its clients at every retrace, at 0x800AD5B4,
//   whether the game draws or not. Each of its four client queues (0x800081B0, 0x8011D978, 0x8011DC28 and 0x801215BC)
//   has a thread of its own that waits on it, and none is the game thread. The driver therefore makes a queue and adds
//   it to the manager's clients, through osCreateMesgQueue at 0x800043E0 and the manager's function at 0x800AD460. It
//   waits on it through osRecvMesg at 0x800021F0. Only the idle thread (0x80006D80, priority 0) has a lower priority
//   than the game thread (0x8011E2B8, priority 11). The idle loop at 0x80000810 is a branch to itself.
// - The audio thread makes no RSP tasks until the function at 0x800014E8 clears a flag. The game calls it under the
//   logos. The driver calls it first.
// - Audio set-up 0 is in force at the snapshot. Each scene's header names a set-up. The set-ups differ in their second
//   reverb. Only sequence 0x2E and sequence 0x6D use that reverb, as runs of all the sequences showed. Sequence 0x2E
//   plays in Ganon's Tower, whose set-up is 6. Its minis reset to set-up 6 first, as the game does.
// - Each room of Ganon's Tower calls the function at 0x800C5CD8 with its level in a0: 7 at the entrance, 6 to 1 on the
//   way up, and 0 in Ganondorf's lair. The function queues a pan weight for each channel: 0x7F for level 0 and 0 for
//   the others. For level 7 it sets a timer of 2 frames at 0x80101F50, which the audio update counts down before it
//   sets the volume. For the other levels it sets the volume at once. The volumes are in the table at 0x80101F48: 0x7F,
//   0x50, 0x4B, 0x49, 0x46, 0x44, 0x41 and 0x3C for levels 0 to 7. The volume function at 0x800C5D84 scales sequence
//   player 0, writes a low-pass cutoff to port 4 of channel 15, and gives each channel whose port 5 is set a reverb of
//   min(0x7F, port 5 - volume + 0x7F). It does nothing for the volume that it set last, at 0x801226D0. The state has
//   0xFF there, as the game's start-up leaves it. The game never plays sequence 0x2E without a level. The set has a
//   track for each level and none without. The driver calls the function once, after the tenth audio update from the
//   start of the sequence.
// - The function at 0x800BEFE8 runs the jumps of all scripts. At 0x800BF168 the new script address is in t2 and the
//   script's state in a1. The state of sequence player 0's script is at 0x801294A8.
//
// The set-up of the game's title screen, 0x0A, has 28 voices. Set-up 0 has 24. The game's code can change some
// sequences through the sequence's ports, such as Hyrule Field's music for the player's state. The minis of Hyrule
// Field set its port, as the game does in every frame. The other minis play with the ports unset.
//
// Sequence 0x02 picks each part of Hyrule Field's music as it plays. It has no loop, so the length analysis stops it at
// its limit of 10 minutes.

#include <string>
#include <vector>

#include "rip/driver.h"
#include "rip/games/games.h"
#include "rip/mips.h"

namespace usf::rip::games
{
namespace
{

constexpr uint32_t kEnableAudio = 0x800014E8;
constexpr uint32_t kFrame = 0x800A1D68;
constexpr uint32_t kAudioUpdate = 0x800C4490;
constexpr uint32_t kQueueCommand = 0x800CB2B0;
constexpr uint32_t kSequenceJump = 0x800BF168;
constexpr uint32_t kSequenceScriptState = 0x801294A8;

constexpr uint32_t kSetStereo = 0xE0000000;
constexpr uint32_t kResetToSetup6 = 0xF0000006;
constexpr uint32_t kGanonsTower = 0x2E;
constexpr uint32_t kSetTowerLevel = 0x800C5CD8;

// The last lines of the frame buffer at the end of RAM. Only the RDP writes there, and it draws nothing after the
// snapshot.
constexpr uint32_t kDriverAddress = 0x803FF000;

// The driver's retrace client, 3 KB after the driver's start. The driver must end before it. Setting `end` to it
// makes BuildCallDriver check that.
constexpr RetraceClient kRetraceClient = {
    .address = kDriverAddress + 0xC00, .create_queue = 0x800043E0, .add_client = 0x800AD460, .manager = 0x8011E038};
constexpr uint32_t kRecvMesg = 0x800021F0;

struct Title
{
    uint32_t number;
    const char* name;
};

constexpr Title kTitles[] = {
    {0x02, "Sequence 0x02: Hyrule Field"},
    {0x03, "Sequence 0x03: Hyrule Field, opening"},
    {0x04, "Sequence 0x04: Hyrule Field, part 1"},
    {0x05, "Sequence 0x05: Hyrule Field, part 2"},
    {0x06, "Sequence 0x06: Hyrule Field, part 3"},
    {0x07, "Sequence 0x07: Hyrule Field, part 4"},
    {0x08, "Sequence 0x08: Hyrule Field, part 5"},
    {0x09, "Sequence 0x09: Hyrule Field, part 6"},
    {0x0A, "Sequence 0x0A: Hyrule Field, part 7"},
    {0x0B, "Sequence 0x0B: Hyrule Field, part 8"},
    {0x0C, "Sequence 0x0C: Hyrule Field, part 9"},
    {0x0D, "Sequence 0x0D: Hyrule Field, part 10"},
    {0x0E, "Sequence 0x0E: Hyrule Field, part 11"},
    {0x0F, "Sequence 0x0F: Hyrule Field, enemies near, opening"},
    {0x10, "Sequence 0x10: Hyrule Field, enemies near, part 1"},
    {0x11, "Sequence 0x11: Hyrule Field, enemies near, part 2"},
    {0x12, "Sequence 0x12: Hyrule Field, enemies near, part 3"},
    {0x13, "Sequence 0x13: Hyrule Field, enemies near, part 4"},
    {0x14, "Sequence 0x14: Hyrule Field, standing still, part 1"},
    {0x15, "Sequence 0x15: Hyrule Field, standing still, part 2"},
    {0x16, "Sequence 0x16: Hyrule Field, standing still, part 3"},
    {0x17, "Sequence 0x17: Hyrule Field, standing still, part 4"},
    {0x18, "Sequence 0x18: Dodongo's Cavern and other caves"},
    {0x19, "Sequence 0x19: Kakariko Village, as an adult"},
    {0x1A, "Sequence 0x1A: battle"},
    {0x1B, "Sequence 0x1B: boss battle"},
    {0x1C, "Sequence 0x1C: Inside the Deku Tree"},
    {0x1D, "Sequence 0x1D: Market"},
    {0x1E, "Sequence 0x1E: title screen"},
    {0x1F, "Sequence 0x1F: houses"},
    {0x20, "Sequence 0x20: game over"},
    {0x21, "Sequence 0x21: boss defeated"},
    {0x22, "Sequence 0x22: item found"},
    {0x23, "Sequence 0x23: Ganondorf appears"},
    {0x24, "Sequence 0x24: Heart Container found"},
    {0x25, "Sequence 0x25: Prelude of Light"},
    {0x26, "Sequence 0x26: Inside Jabu-Jabu's Belly"},
    {0x27, "Sequence 0x27: Kakariko Village, as a child"},
    {0x28, "Sequence 0x28: Great Fairy's Fountain"},
    {0x29, "Sequence 0x29: Castle Courtyard, Zelda"},
    {0x2A, "Sequence 0x2A: Fire Temple"},
    {0x2B, "Sequence 0x2B: treasure chest opens"},
    {0x2C, "Sequence 0x2C: Forest Temple"},
    {0x2D, "Sequence 0x2D: Castle Courtyard, guards"},
    {0x2E, nullptr}, // A track for each level of Ganon's Tower.
    {0x2F, "Sequence 0x2F: Lon Lon Ranch"},
    {0x30, "Sequence 0x30: Goron City"},
    {0x31, "Sequence 0x31: Hyrule Field, morning"},
    {0x32, "Sequence 0x32: Spiritual Stone found"},
    {0x33, "Sequence 0x33: Bolero of Fire"},
    {0x34, "Sequence 0x34: Minuet of Forest"},
    {0x35, "Sequence 0x35: Serenade of Water"},
    {0x36, "Sequence 0x36: Requiem of Spirit"},
    {0x37, "Sequence 0x37: Nocturne of Shadow"},
    {0x38, "Sequence 0x38: mini-boss battle"},
    {0x39, "Sequence 0x39: small item found"},
    {0x3A, "Sequence 0x3A: Temple of Time"},
    {0x3B, "Sequence 0x3B: puzzle solved"},
    {0x3C, "Sequence 0x3C: Kokiri Forest"},
    {0x3D, "Sequence 0x3D: Fairy Ocarina found"},
    {0x3E, "Sequence 0x3E: Lost Woods"},
    {0x3F, "Sequence 0x3F: Spirit Temple"},
    {0x40, "Sequence 0x40: horse race"},
    {0x41, "Sequence 0x41: horse race won"},
    {0x42, "Sequence 0x42: Ingo"},
    {0x43, "Sequence 0x43: Medallion found"},
    {0x44, "Sequence 0x44: Saria's Song"},
    {0x45, "Sequence 0x45: Epona's Song"},
    {0x46, "Sequence 0x46: Zelda's Lullaby"},
    {0x47, "Sequence 0x47: Sun's Song"},
    {0x48, "Sequence 0x48: Song of Time"},
    {0x49, "Sequence 0x49: Song of Storms"},
    {0x4A, "Sequence 0x4A: Navi's opening"},
    {0x4B, "Sequence 0x4B: the Deku Tree speaks"},
    {0x4C, "Sequence 0x4C: Windmill"},
    {0x4D, "Sequence 0x4D: the story of Hyrule"},
    {0x4E, "Sequence 0x4E: mini-games"},
    {0x4F, "Sequence 0x4F: Sheik"},
    {0x50, "Sequence 0x50: Zora's Domain"},
    {0x51, "Sequence 0x51: someone appears"},
    {0x52, "Sequence 0x52: Link as an adult"},
    {0x53, "Sequence 0x53: Master Sword"},
    {0x54, "Sequence 0x54: Ganondorf's introduction"},
    {0x55, "Sequence 0x55: shops"},
    {0x56, "Sequence 0x56: Chamber of the Sages"},
    {0x58, "Sequence 0x58: Ice Cavern"},
    {0x59, "Sequence 0x59: Door of Time"},
    {0x5A, "Sequence 0x5A: Kaepora Gaebora"},
    {0x5B, "Sequence 0x5B: Shadow Temple"},
    {0x5C, "Sequence 0x5C: Water Temple"},
    {0x5D, "Sequence 0x5D: bridge to Ganon's Castle"},
    {0x5E, "Sequence 0x5E: seal of the Sages"},
    {0x5F, "Sequence 0x5F: Gerudo Valley"},
    {0x60, "Sequence 0x60: Potion Shop"},
    {0x61, "Sequence 0x61: Kotake and Koume"},
    {0x62, "Sequence 0x62: escape from Ganon's Castle"},
    {0x63, "Sequence 0x63: Inside Ganon's Castle"},
    {0x64, "Sequence 0x64: Ganondorf battle"},
    {0x65, "Sequence 0x65: Ganon battle"},
    {0x66, "Sequence 0x66: Ocarina of Time"},
    {0x67, "Sequence 0x67: staff roll, part 1"},
    {0x68, "Sequence 0x68: staff roll, part 2"},
    {0x69, "Sequence 0x69: staff roll, part 3"},
    {0x6A, "Sequence 0x6A: staff roll, part 4"},
    {0x6B, "Sequence 0x6B: Volvagia battle"},
    {0x6C, "Sequence 0x6C: timed mini-game"},
};

// Tracks that play a sequence in a state that the game's code sets after the start. Each track's number is the game's
// command, which the driver's last call queues. The track's mini turns the call for Ganon's Tower into one that fires
// for the number's low byte and starts the sequence first.
//
// - Once a frame, the code at 0x800C7600 queues 0x7002000V, which writes Link's state to port 2 of sequence player 0: 0
//   when he moves, 1 with enemies near and 2 when he stands still. Sequence 0x02 picks its next part by the port. Runs
//   of the game with sequence 0x02 in place of the title's music start the same for each value. Later they differ. With
//   the port unset they differ too. The track for Hyrule Field therefore sets 0.
// - The function at 0x800C776C sets the volumes of Lon Lon Ranch's channels by Link's distance from Malon: channels 0
//   and 1 hold her voice, and channel 13 a melody. Within 200 units the melody is at 0. Beyond 2000 units her voice is
//   at 0. That state sounds almost the same as the track without the call. The set leaves it out. Command 0x6003CCVV
//   sets channel CC's volume to VV over 3 frames.
// - The function at 0x800C72BC queues a write to a port and then starts a sequence. The file select's code calls it for
//   sequence 0x57 with 1 for port 7. In a run that pressed Start at the title, the call came at 0x803B3CE8. Sequence
//   0x57 points to sequence 0x28. With the port at 1, sequence 0x28 leaves out the harp's introduction. The track plays
//   sequence 0x57, as the game does. The scene's function at 0x800C69CC writes 1 to port 7 for a sequence whose flags
//   at 0x80101FA4 hold 0x40, and -1 for the rest. The Great Fairy's Fountain plays sequence 0x28 with -1. Runs of the
//   scene's function with sequence 0x57 patched in at both of its callers, 0x80061F8C and 0x80061FC4, part from the
//   start with the port at 1 and at -1. Sequence 0x56, the Chamber of the Sages, has the same flag. The port's value
//   doesn't change what sequence 0x56 plays. The set has no track for that state.
struct CommandVersion
{
    uint32_t sequence;
    uint32_t command;
    const char* name;
};

constexpr CommandVersion kHyruleFieldMoving = {0x02, 0x70020000, nullptr};

constexpr CommandVersion kCommandVersions[] = {
    {0x02, 0x70020001, "Sequence 0x02: Hyrule Field, enemies near"},
    {0x02, 0x70020002, "Sequence 0x02: Hyrule Field, standing still"},
    {0x2F, 0x60030D00, "Sequence 0x2F: Lon Lon Ranch, close to Malon"},
    {0x57, 0x70070001, "Sequence 0x28: file select, without the harp's introduction"},
};

std::vector<RdramPatch> CommandVersionPatches(const Driver& driver, const CommandVersion& version)
{
    return {DriverWordPatch(driver, mips::Addiu(mips::kT0, mips::kZero, kGanonsTower),
                            mips::Addiu(mips::kT0, mips::kZero, version.command & 0xFF)),
            DriverWordPatch(driver, mips::Lui(mips::kA0, kResetToSetup6 >> 16), mips::Lui(mips::kA0, 0)),
            DriverWordPatch(driver, mips::Ori(mips::kA0, mips::kA0, kResetToSetup6 & 0xFFFF),
                            mips::Ori(mips::kA0, mips::kA0, static_cast<uint16_t>(version.sequence)))};
}

// The driver calls the level function of Ganon's Tower for no track. The mini of each level patches two words of the
// driver: one turns the call on, and one sets the level.
constexpr uint32_t kCallOff = 1;
constexpr uint32_t kEntranceLevel = 7;

// The title of the track for a level of Ganon's Tower.
std::string TowerLevelName(uint32_t level)
{
    if (level == kEntranceLevel)
    {
        return "Sequence 0x2E: Ganon's Tower, entrance";
    }
    if (level == 0)
    {
        return "Sequence 0x2E: Ganon's Tower, Ganondorf's lair";
    }

    return "Sequence 0x2E: Ganon's Tower, level " + std::to_string(level);
}

std::vector<RdramPatch> TowerLevelPatches(const Driver& driver, uint32_t level)
{
    return {
        DriverWordPatch(driver, mips::Addiu(mips::kT0, mips::kZero, kCallOff), mips::Addiu(mips::kT0, mips::kZero, 0)),
        DriverWordPatch(driver, mips::Addiu(mips::kA0, mips::kZero, kEntranceLevel),
                        mips::Addiu(mips::kA0, mips::kZero, static_cast<uint16_t>(level)))};
}

} // namespace

GameProfile NusCzleUsaRev2()
{
    GameProfile profile;
    profile.game = "The Legend of Zelda: Ocarina of Time";
    profile.serial = "NUS-CZLE-USA-rev2";
    profile.sha1 = "41b3bdc48d98c48529219919015a1af22f5057c2";
    profile.year = "1998";
    profile.cic = n64::Cic::k6105;
    profile.sound_driver = "Ocarina of Time sound engine";
    profile.controller = true;

    profile.snapshot_address = kFrame;
    profile.snapshot_hit = 1;
    profile.quiet_tries = 1;
    profile.boot_seconds = 5.0;

    CallDriverOptions driver;
    driver.address = kDriverAddress;
    driver.end = kRetraceClient.address;
    driver.start_calls = {{.function = kEnableAudio},
                          {.function = kQueueCommand, .arguments = {{.value = kSetStereo}}},
                          {.function = kQueueCommand,
                           .arguments = {{.value = kResetToSetup6}},
                           .track_mask = 0xFF,
                           .track_value = kGanonsTower},
                          {.function = kQueueCommand, .arguments = {{.track = true}}}};
    // Between frames the driver waits for three retrace messages on a queue of its own. The idle thread runs meanwhile.
    for (const GameCall& call : AddRetraceClient(kRetraceClient))
    {
        driver.start_calls.push_back(call);
    }
    driver.wait_call = MessageWait(kRecvMesg, kRetraceClient.address);
    // The level call: after the tenth audio update, for no track until a mini's patch turns it on.
    driver.frame_calls = {{.function = kAudioUpdate},
                          {.function = kSetTowerLevel,
                           .arguments = {{.value = kEntranceLevel}},
                           .track_value = kCallOff,
                           .first_frame = 9,
                           .frames = 10}};
    driver.fields = 3;
    profile.driver = BuildCallDriver(driver);

    profile.loop_watches.push_back({.address = kSequenceJump,
                                    .target_register = mips::kT2,
                                    .match_register = mips::kA1,
                                    .match_value = kSequenceScriptState});

    for (const Title& title : kTitles)
    {
        if (title.number == kGanonsTower)
        {
            for (uint32_t level = kEntranceLevel + 1; level-- > 0;)
            {
                Track track;
                track.number = kGanonsTower;
                track.name = TowerLevelName(level);
                track.rdram_patches = TowerLevelPatches(*profile.driver, level);
                profile.tracks.push_back(track);
            }
            continue;
        }

        Track track;
        track.number = title.number;
        track.name = title.name;
        if (title.number == kHyruleFieldMoving.sequence)
        {
            track.number = kHyruleFieldMoving.command;
            track.rdram_patches = CommandVersionPatches(*profile.driver, kHyruleFieldMoving);
        }
        profile.tracks.push_back(track);
    }

    for (const CommandVersion& version : kCommandVersions)
    {
        Track track;
        track.number = version.command;
        track.name = version.name;
        track.rdram_patches = CommandVersionPatches(*profile.driver, version);
        profile.tracks.push_back(track);
    }

    return profile;
}

} // namespace usf::rip::games
