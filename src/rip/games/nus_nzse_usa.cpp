// SPDX-License-Identifier: MIT

// The Legend of Zelda: Majora's Mask (USA): the game's music. The game has no sound test.
//
// Addresses come from the game's code in the image and from runs in usf's core. The game requires the answers of the
// CIC-NUS-6105. They come from a dump of the console's PIF ROM: usfrip --pif FILE. It also requires the Expansion Pak.
// The set therefore has 8 MB of RDRAM.
//
// - The sequence table at 0x801E1630 contains 128 sequences. Sequence 0 contains the sound effects. Sequences 0x23,
//   0x28, 0x56, 0x60 and 0x61 point to sequences 0x16, 0x18, 0x3C, 0x57 and 0x51. The set leaves them out. Sequence
//   0x01 contains ambience and stays silent until the game's code sets its ports. Sequence 0x7A makes no sound. The set
//   leaves both out.
// - The function at 0x801A89A8 takes a 32-bit command in a0 and queues it for the audio update. Command 0x000000NN
//   starts sequence NN on sequence player 0. Command 0xE0000000 sets stereo output. When a scene's music starts, the
//   function at 0x801A25E4 follows that command with 0x7004000V. That command writes the day minus 1 to the player's
//   port 4. The function at 0x800FB758 limits the value to 0 to 2. A sequence's start sets every port to -1, and port 4
//   of player 0 is at 0x8020522C. The profile sends the same commands.
// - Sequences 0x15 to 0x17 contain Clock Town's three days. The game starts sequence 0x1D. That sequence plays one of
//   them for the day in the port. The set reaches them through sequence 0x1D and leaves them out alone.
// - Runs of the game in usf's core, with sequence 0x4D or 0x54 patched in at 0x801A25F4 and 0x801A2640, write 0 to port
//   4. The first-day tracks of those sequences write 0 there too. With -1 in the port, sequence 0x54 falls silent
//   partway through.
// - The game thread (entry 0x801748A0) calls the frame function at 0x80174868 once a frame. That function's work ends
//   with a call of the audio update at 0x8019E014. The audio update reads the queue. Gameplay has a frame of three
//   video fields.
// - The snapshot takes over the game thread at the frame function's first call. Later calls aren't quiet: the audio
//   thread keeps AI's second buffer waiting.
// - The game's interrupt manager at 0x80099EF0 sends a message to each of its clients at every retrace, at 0x80081390,
//   whether the game draws or not. Each of its four client queues (0x801F81C0, 0x801F8500, 0x801FB39C and 0x801FB69C)
//   has a thread of its own that waits on it, and none is the game thread. The driver therefore makes a queue and adds
//   it to the manager's clients, through osCreateMesgQueue at 0x8008F240 and the manager's function at 0x80081250. It
//   waits on it through osRecvMesg at 0x80087ED0. Only the idle thread (0x80099520, priority 0) has a lower priority
//   than the game thread (0x801F8608, priority 9). The idle loop at 0x800805C0 is a branch to itself.
// - A probe of each sequence found none that uses a reverb other than the first. All the game's audio set-ups share
//   that reverb.
// - The function at 0x80197538 runs the jumps of all scripts. At 0x801976B8 the new script address is in t9 and the
//   script's state in a1. The state of sequence player 0's script is at 0x80205148.

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <stdexcept>
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

constexpr uint32_t kFrame = 0x80174868;
constexpr uint32_t kAudioUpdate = 0x8019E014;
constexpr uint32_t kQueueCommand = 0x801A89A8;
constexpr uint32_t kSequenceJump = 0x801976B8;
constexpr uint32_t kSequenceScriptState = 0x80205148;

constexpr uint32_t kSetStereo = 0xE0000000;

// A track's number contains the day's port value above the sequence's number.
constexpr uint32_t kDayShift = 8;
constexpr uint32_t kDayMask = 0xFF00;
constexpr uint32_t kAllMask = 0xFFFF;
constexpr uint32_t kSetDayPort = 0x70040000;

// Command 0x70000001 writes 1 to port 0. Command 0x6003CCVV sets channel CC's volume to VV over 3 frames.
constexpr uint32_t kSetDawnPort = 0x70000001;
constexpr uint32_t kSetPort7 = 0x70070000;
constexpr uint32_t kSetChannelVolume = 0x60030000;
constexpr uint32_t kChannelShift = 8;

// The end of the Expansion Pak's memory. Runs of the game leave zeros there.
constexpr uint32_t kDriverAddress = 0x807FF000;

// The driver's retrace client, 3 KB after the driver's start. The driver must end before it. Setting `end` to it
// makes BuildCallDriver check that.
constexpr RetraceClient kRetraceClient = {
    .address = kDriverAddress + 0xC00, .create_queue = 0x8008F240, .add_client = 0x80081250, .manager = 0x80099EF0};
constexpr uint32_t kRecvMesg = 0x80087ED0;

// The sequences that play differently for the day in port 4. Sequence 0x4D plays alike on the second and third days.
// Values above 2 never reach the port.
struct DayVersion
{
    uint32_t sequence;
    uint32_t day;

    constexpr uint32_t Number() const
    {
        return (day << kDayShift) | sequence;
    }
};

constexpr DayVersion kDayVersions[] = {
    {0x1D, 1}, // Clock Town, second day.
    {0x1D, 2}, // Clock Town, third day.
    {0x4D, 1}, // New Wave Bossa Nova, second and third days.
    {0x54, 1}, // Ballad of the Wind Fish, second day.
    {0x54, 2}, // Ballad of the Wind Fish, third day.
};

// The sequences whose first day sounds different with 0 in port 4 and with -1. Their first-day tracks get
// FirstDayPatches. Sequences 0x1D, 0x67 and 0x68 also read the port. For them, 0 and -1 play the same.
constexpr uint32_t kFirstDaySequences[] = {0x4D, 0x54};

// Tracks whose minis turn the driver's two day calls into other commands. Each call then fires for every track whose
// number contains no day. 0 leaves a call as it is.
//
// - The function at 0x800FB758 calls 0x801A2544 in place of 0x801A25E4 when the time is from 0x4000 to 0xB71C and the
//   byte at 0x801BDBC8 isn't 0. That function sends 0x70000001 as well as the day. Sequences 0x02 and 0x1D read port 0
//   and play a dawn piece for 1. A probe of all the sequences found no other that reads port 0.
// - The function at 0x801A36F0 sets the volumes of Romani Ranch's channels by Link's distance from Romani: channels 0
//   and 1 play her voice, and channel 13 a melody. Within 200 units the melody is at 0. Beyond 2000 units her voice is
//   at 0. In between is a mix. On each day, the state beyond 2000 units plays as the track without the commands does.
//   The set leaves that state out.
struct CommandVersion
{
    uint32_t sequence;
    uint32_t commands[2];
};

constexpr uint32_t ChannelVolume(uint32_t channel, uint32_t volume)
{
    return kSetChannelVolume | (channel << kChannelShift) | volume;
}

constexpr CommandVersion kCommandVersions[] = {
    {0x02, {kSetDawnPort, kSetDayPort}},     // Termina Field, at dawn.
    {0x1D, {kSetDawnPort, kSetDayPort}},     // Clock Town, dawn of the first day.
    {0x1D, {kSetDawnPort, kSetDayPort | 1}}, // Clock Town, dawn of the second day.
    {0x1D, {kSetDawnPort, kSetDayPort | 2}}, // Clock Town, dawn of the third day.
    {0x2F, {ChannelVolume(13, 0), 0}},       // Romani Ranch, close to Romani.
    {0x18, {kSetPort7 | 1, 0}},              // The file select, without the harp's introduction.
};

// The fanfares that play a song back once Link has played it. The function at 0x801A3000 starts the fanfare and writes
// a value for Link's form to port 7, from the table at 0x801D039C: 0x35 for Link and the Fierce Deity, 0x5C for Goron
// Link, 0x5D for Zora Link and 0x5E for Deku Link. These fourteen sequences read port 7. With 0x35 in the port, they
// play as they do with -1. The other three values change what plays. Each form gets a track, and the track without a
// value plays Link's version. The scene's function writes 1 to port 7 for sequence 0x18, and -1 for sequence 0x28, the
// Fairy Fountain. That sequence points to 0x18. Runs of the two play differently from the start.
constexpr uint32_t kForms[] = {0x5C, 0x5D, 0x5E};

constexpr uint32_t kSongFanfares[] = {0x32, 0x33, 0x34, 0x35, 0x47, 0x48, 0x49,
                                      0x4A, 0x51, 0x5B, 0x5C, 0x5D, 0x5E, 0x5F};

// The sequences of the table, and those that the set leaves out: the ambience of 0x01, Clock Town's days, the five
// sequences that point to others, and the silent 0x7A. The set reaches the days through sequence 0x1D.
constexpr uint32_t kSequences = 0x80;
constexpr uint32_t kLeftOut[] = {0x01, 0x15, 0x16, 0x17, 0x23, 0x28, 0x56, 0x60, 0x61, 0x7A};

// The driver's calls send days 1 and 2. These patches change the call for day 2 into a call for day 0, in the minis of
// the first-day tracks that require the command. A further call in the driver would change the library and every mini.
std::vector<RdramPatch> FirstDayPatches(const Driver& driver)
{
    constexpr uint32_t kDay2 = 2;

    return {DriverWordPatch(driver, mips::Addiu(mips::kT0, mips::kZero, kDay2 << kDayShift),
                            mips::Addiu(mips::kT0, mips::kZero, 0)),
            DriverWordPatch(driver, mips::Ori(mips::kA0, mips::kA0, kDay2), mips::Ori(mips::kA0, mips::kA0, 0))};
}

// The patches that make the driver's call for `day` queue `command` for every track whose number contains no day.
std::vector<RdramPatch> DayCallPatches(const Driver& driver, uint32_t day, uint32_t command)
{
    const RdramPatch match =
        DriverWordPatch(driver, mips::Addiu(mips::kT0, mips::kZero, static_cast<uint16_t>(day << kDayShift)),
                        mips::Addiu(mips::kT0, mips::kZero, 0));
    const RdramPatch lower = DriverWordPatch(driver, mips::Ori(mips::kA0, mips::kA0, static_cast<uint16_t>(day)),
                                             mips::Ori(mips::kA0, mips::kA0, command & 0xFFFF));

    // A LUI before the ORI loads the command's upper half.
    const uint32_t upper = lower.address - 4;
    if (lower.address <= driver.address ||
        driver.code[(upper - driver.address) / 4] != mips::Lui(mips::kA0, kSetDayPort >> 16))
    {
        throw std::logic_error("Majora's Mask's driver doesn't load the day command with a LUI and an ORI");
    }

    return {match, {.address = upper, .words = {mips::Lui(mips::kA0, command >> 16), lower.words[0]}}};
}

// Adds a track for each sequence, apart from kLeftOut. The first-day tracks of kFirstDaySequences get
// FirstDayPatches.
void AddTitleTracks(const Driver& driver, std::vector<Track>& tracks)
{
    for (uint32_t number = 1; number < kSequences; number++)
    {
        if (std::ranges::find(kLeftOut, number) != std::end(kLeftOut))
        {
            continue;
        }

        Track track;
        track.number = number;
        track.name = NumberTitle("Sequence", number, TitleBase::kHex);
        if (std::ranges::find(kFirstDaySequences, number) != std::end(kFirstDaySequences))
        {
            track.rdram_patches = FirstDayPatches(driver);
        }
        tracks.push_back(track);
    }
}

// Adds a track for each of kDayVersions.
void AddDayVersionTracks(std::vector<Track>& tracks)
{
    for (const DayVersion& version : kDayVersions)
    {
        Track track;
        track.number = version.Number();
        track.name = NumberTitle("Sequence", version.sequence, TitleBase::kHex);
        tracks.push_back(track);
    }
}

// Adds a track for each of kCommandVersions, with the patches that turn the day calls into the commands of the version.
void AddCommandVersionTracks(const Driver& driver, std::vector<Track>& tracks)
{
    for (const CommandVersion& version : kCommandVersions)
    {
        Track track;
        track.number = version.sequence;
        track.name = NumberTitle("Sequence", version.sequence, TitleBase::kHex);
        for (uint32_t day = 1; day <= 2; day++)
        {
            const uint32_t command = version.commands[day - 1];
            if (command != 0)
            {
                const std::vector<RdramPatch> patches = DayCallPatches(driver, day, command);
                track.rdram_patches.insert(track.rdram_patches.end(), patches.begin(), patches.end());
            }
        }
        tracks.push_back(track);
    }
}

// Adds a track for each form of each of kSongFanfares.
void AddFanfareTracks(const Driver& driver, std::vector<Track>& tracks)
{
    for (const uint32_t sequence : kSongFanfares)
    {
        for (const uint32_t form : kForms)
        {
            Track track;
            track.number = sequence;
            track.name = NumberTitle("Sequence", sequence, TitleBase::kHex);
            track.rdram_patches = DayCallPatches(driver, 1, kSetPort7 | form);
            tracks.push_back(track);
        }
    }
}

} // namespace

GameProfile NusNzseUsa()
{
    GameProfile profile;
    profile.game = "The Legend of Zelda: Majora's Mask";
    profile.serial = "NUS-NZSE-USA";
    profile.sha1 = "d6133ace5afaa0882cf214cf88daba39e266c078";
    profile.cic = ultralib::machine::Cic::k6105;
    profile.sound_driver = "Majora's Mask sound engine";
    profile.controller = true;
    profile.rdram_size = ultralib::machine::kRdramSize8;

    profile.snapshot_address = kFrame;
    profile.snapshot_hit = 1;
    profile.quiet_tries = 1;
    profile.boot_seconds = 5.0;

    CallDriverOptions driver;
    driver.address = kDriverAddress;
    driver.end = kRetraceClient.address;
    driver.start_calls = {{.function = kQueueCommand, .arguments = {{.value = kSetStereo}}},
                          {.function = kQueueCommand, .arguments = {{.track = true}}, .track_mask = kDayMask}};
    for (const DayVersion& version : kDayVersions)
    {
        driver.start_calls.push_back({.function = kQueueCommand,
                                      .arguments = {{.value = version.sequence}},
                                      .track_mask = kAllMask,
                                      .track_value = version.Number()});
    }

    for (const uint32_t day : {1u, 2u})
    {
        driver.start_calls.push_back({.function = kQueueCommand,
                                      .arguments = {{.value = kSetDayPort | day}},
                                      .track_mask = kDayMask,
                                      .track_value = day << kDayShift});
    }

    // Between frames the driver waits for three retrace messages on a queue of its own. The idle thread runs meanwhile.
    for (const GameCall& call : AddRetraceClient(kRetraceClient))
    {
        driver.start_calls.push_back(call);
    }

    driver.frame_calls = {{.function = kAudioUpdate}};
    driver.wait_call = MessageWait(kRecvMesg, kRetraceClient.address);
    driver.fields = 3;
    profile.driver = BuildCallDriver(driver);

    profile.loop_watches.push_back({.address = kSequenceJump,
                                    .target_register = mips::kT9,
                                    .match_register = mips::kA1,
                                    .match_value = kSequenceScriptState});

    AddTitleTracks(*profile.driver, profile.tracks);
    AddDayVersionTracks(profile.tracks);
    AddCommandVersionTracks(*profile.driver, profile.tracks);
    AddFanfareTracks(*profile.driver, profile.tracks);

    return profile;
}

} // namespace usf::rip::games
