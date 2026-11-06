// SPDX-License-Identifier: MIT

// The Legend of Zelda: Majora's Mask (USA): the game's music. The game has no sound test. Each title names the place or
// the event where the game plays the sequence.
//
// Addresses come from the game's code in the image and from runs in usf's core. The game requires the CIC-NUS-6105's
// answers, which come from a dump of the console's PIF ROM: usfrip --pif FILE. It also requires the Expansion Pak, so
// the set holds 8 MB of RDRAM.
//
// - The sequence table at 0x801E1630 holds 128 sequences. Sequence 0 holds the sound effects. Sequences 0x23, 0x28,
//   0x56, 0x60 and 0x61 point to sequences 0x16, 0x18, 0x3C, 0x57 and 0x51, so the set leaves them out.
//   Sequence 0x01 holds ambience and stays silent until the game's code sets its ports. Sequence 0x7A makes no sound.
//   The set leaves both out.
// - The function at 0x801A89A8 takes a 32-bit command in a0 and queues it for the audio update. Command 0x000000NN
//   starts sequence NN on sequence player 0. Command 0xE0000000 sets stereo output. When a scene's music starts, the
//   function at 0x801A25E4 follows that command with 0x7004000V, which writes the day minus 1 to the player's port 4.
//   The function at 0x800FB758 limits the value to 0 to 2. A sequence's start sets every port to -1, and port 4 of
//   player 0 is at 0x8020522C. The profile sends the same commands.
// - Sequences 0x15 to 0x17 hold Clock Town's three days. The game starts sequence 0x1D, which plays one of them for the
//   day in the port. The set reaches them through sequence 0x1D and leaves them out alone.
// - Runs of the game in usf's core, with sequence 0x4D or 0x54 patched in at 0x801A25F4 and 0x801A2640, write 0 to
//   port 4. The first-day tracks of those sequences write 0 there too. With -1 in the port, sequence 0x54 falls silent
//   partway through.
// - The game thread (entry 0x801748A0) calls the frame function at 0x80174868 once a frame. That function's work ends
//   with a call of the audio update at 0x8019E014, which reads the queue. Gameplay has a frame of three video fields.
// - The snapshot takes over the game thread at the frame function's first call. Later calls aren't quiet: the audio
//   thread keeps AI's second buffer waiting.
// - A probe of each sequence found none that uses a reverb other than the first. All the game's audio set-ups share
//   that reverb.
// - The function at 0x80197538 runs the jumps of all scripts. At 0x801976B8 the new script address is in t9 and the
//   script's state in a1. The state of sequence player 0's script is at 0x80205148.

#include <algorithm>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#include "rip/driver.h"
#include "rip/games/games.h"
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

// A track's number holds the day's port value above the sequence's number.
constexpr uint32_t kDayShift = 8;
constexpr uint32_t kDayMask = 0xFF00;
constexpr uint32_t kAllMask = 0xFFFF;
constexpr uint32_t kSetDayPort = 0x70040000;

// Command 0x70000001 writes 1 to port 0. Command 0x6003CCVV sets channel CC's volume to VV over 3 frames.
constexpr uint32_t kSetDawnPort = 0x70000001;
constexpr uint32_t kSetPort7 = 0x70070000;
constexpr uint32_t kSetChannelVolume = 0x60030000;
constexpr uint32_t kChannelShift = 8;

// The end of the Expansion Pak's memory. Runs of the game hold zeros there.
constexpr uint32_t kDriverAddress = 0x807FF000;

struct Title
{
    uint32_t number;
    const char* name;
};

// The sequences that play differently for the day in port 4. Sequence 0x4D plays alike on the second and third days.
// Values above 2 never reach the port.
struct DayVersion
{
    uint32_t sequence;
    uint32_t day;
    const char* name;

    constexpr uint32_t Number() const
    {
        return (day << kDayShift) | sequence;
    }
};

constexpr DayVersion kDayVersions[] = {
    {0x1D, 1, "Sequence 0x1D: Clock Town, second day"},
    {0x1D, 2, "Sequence 0x1D: Clock Town, third day"},
    {0x4D, 1, "Sequence 0x4D: New Wave Bossa Nova, second and third days"},
    {0x54, 1, "Sequence 0x54: Ballad of the Wind Fish, second day"},
    {0x54, 2, "Sequence 0x54: Ballad of the Wind Fish, third day"},
};

// The sequences whose first day sounds different with 0 in port 4 and with -1. Their first-day tracks get
// FirstDayPatches. Sequences 0x1D, 0x67 and 0x68 also read the port. For them, 0 and -1 play the same.
constexpr uint32_t kFirstDaySequences[] = {0x4D, 0x54};

// Tracks whose minis turn the driver's two day calls into other commands. Each call then fires for every track
// whose number holds no day. 0 leaves a call as it is.
//
// - The function at 0x800FB758 calls 0x801A2544 in place of 0x801A25E4 when the time is from 0x4000 to 0xB71C and
//   the byte at 0x801BDBC8 isn't 0. That function sends 0x70000001 as well as the day. Sequences 0x02 and 0x1D read
//   port 0 and play a dawn piece for 1. A probe of all the sequences found no other that reads port 0.
// - The function at 0x801A36F0 sets the volumes of Romani Ranch's channels by Link's distance from Romani: channels 0
//   and 1 hold her voice, and channel 13 a melody. Within 200 units the melody is at 0. Beyond 2000 units her voice is
//   at 0. In between is a mix. On each day, the state beyond 2000 units plays as the track without the commands does.
//   The set leaves that state out.
struct CommandVersion
{
    uint32_t sequence;
    uint32_t commands[2];
    const char* name;
};

constexpr uint32_t ChannelVolume(uint32_t channel, uint32_t volume)
{
    return kSetChannelVolume | (channel << kChannelShift) | volume;
}

constexpr CommandVersion kCommandVersions[] = {
    {0x02, {kSetDawnPort, kSetDayPort}, "Sequence 0x02: Termina Field, at dawn"},
    {0x1D, {kSetDawnPort, kSetDayPort}, "Sequence 0x1D: Clock Town, dawn of the first day"},
    {0x1D, {kSetDawnPort, kSetDayPort | 1}, "Sequence 0x1D: Clock Town, dawn of the second day"},
    {0x1D, {kSetDawnPort, kSetDayPort | 2}, "Sequence 0x1D: Clock Town, dawn of the third day"},
    {0x2F, {ChannelVolume(13, 0), 0}, "Sequence 0x2F: Romani Ranch, close to Romani"},
    {0x18, {kSetPort7 | 1, 0}, "Sequence 0x18: file select, without the harp's introduction"},
};

// The fanfares that play a song back once Link has played it. The function at 0x801A3000 starts the fanfare and writes
// a value for Link's form to port 7, from the table at 0x801D039C: 0x35 for Link and the Fierce Deity, 0x5C for Goron
// Link, 0x5D for Zora Link and 0x5E for Deku Link. These fourteen sequences read port 7. With 0x35 in the port, they
// play as they do with -1. The other three values change what plays. Each form gets a track, and the track without a
// value plays Link's version. The scene's function writes 1 to port 7 for sequence 0x18, and -1 for sequence 0x28, the
// Fairy Fountain, which points to 0x18. Runs of the two play differently from the start.
struct Form
{
    uint32_t value;
    const char* name;
};

constexpr Form kForms[] = {{0x5C, ", as Goron Link"}, {0x5D, ", as Zora Link"}, {0x5E, ", as Deku Link"}};

constexpr uint32_t kSongFanfares[] = {0x32, 0x33, 0x34, 0x35, 0x47, 0x48, 0x49,
                                      0x4A, 0x51, 0x5B, 0x5C, 0x5D, 0x5E, 0x5F};

constexpr Title kTitles[] = {
    {0x02, "Sequence 0x02: Termina Field"},
    {0x03, "Sequence 0x03: chase"},
    {0x04, "Sequence 0x04: Majora's theme"},
    {0x05, "Sequence 0x05: Clock Tower"},
    {0x06, "Sequence 0x06: Stone Tower Temple"},
    {0x07, "Sequence 0x07: Stone Tower Temple, inverted"},
    {0x08, "Sequence 0x08: failure, part 1"},
    {0x09, "Sequence 0x09: failure, part 2"},
    {0x0A, "Sequence 0x0A: Happy Mask Salesman"},
    {0x0B, "Sequence 0x0B: Song of Healing"},
    {0x0C, "Sequence 0x0C: Southern Swamp"},
    {0x0D, "Sequence 0x0D: aliens' attack"},
    {0x0E, "Sequence 0x0E: boat cruise"},
    {0x0F, "Sequence 0x0F: Sharp's curse"},
    {0x10, "Sequence 0x10: Great Bay Coast"},
    {0x11, "Sequence 0x11: Ikana Canyon"},
    {0x12, "Sequence 0x12: Deku Palace"},
    {0x13, "Sequence 0x13: Mountain Village"},
    {0x14, "Sequence 0x14: Pirates' Fortress"},
    {0x18, "Sequence 0x18: Fairy Fountain"},
    {0x19, "Sequence 0x19: puzzle solved"},
    {0x1A, "Sequence 0x1A: battle"},
    {0x1B, "Sequence 0x1B: boss battle"},
    {0x1C, "Sequence 0x1C: Woodfall Temple"},
    {0x1D, "Sequence 0x1D: Clock Town, first day"},
    {0x1E, "Sequence 0x1E: opening"},
    {0x1F, "Sequence 0x1F: inside a house"},
    {0x20, "Sequence 0x20: game over"},
    {0x21, "Sequence 0x21: boss defeated"},
    {0x22, "Sequence 0x22: item found"},
    {0x24, "Sequence 0x24: Heart Container found"},
    {0x25, "Sequence 0x25: timed mini-game"},
    {0x26, "Sequence 0x26: Goron race"},
    {0x27, "Sequence 0x27: Music Box House"},
    {0x29, "Sequence 0x29: Zelda's Lullaby"},
    {0x2A, "Sequence 0x2A: Rosa Sisters"},
    {0x2B, "Sequence 0x2B: treasure chest opens"},
    {0x2C, "Sequence 0x2C: Marine Research Lab"},
    {0x2D, "Sequence 0x2D: the Giants"},
    {0x2E, "Sequence 0x2E: Song of Storms"},
    {0x2F, "Sequence 0x2F: Romani Ranch"},
    {0x30, "Sequence 0x30: Goron Village"},
    {0x31, "Sequence 0x31: Mayor's Office"},
    {0x32, "Sequence 0x32: Epona's Song"},
    {0x33, "Sequence 0x33: Sun's Song"},
    {0x34, "Sequence 0x34: Song of Time"},
    {0x35, "Sequence 0x35: Song of Storms, ocarina"},
    {0x36, "Sequence 0x36: Zora Hall"},
    {0x37, "Sequence 0x37: new mask found"},
    {0x38, "Sequence 0x38: mini-boss battle"},
    {0x39, "Sequence 0x39: small item found"},
    {0x3A, "Sequence 0x3A: Astral Observatory"},
    {0x3B, "Sequence 0x3B: caves"},
    {0x3C, "Sequence 0x3C: Milk Bar"},
    {0x3D, "Sequence 0x3D: Zelda appears"},
    {0x3E, "Sequence 0x3E: Saria's Song"},
    {0x3F, "Sequence 0x3F: Goron race won"},
    {0x40, "Sequence 0x40: horse race"},
    {0x41, "Sequence 0x41: horse race won"},
    {0x42, "Sequence 0x42: Gorman Bros."},
    {0x43, "Sequence 0x43: Potion Shop"},
    {0x44, "Sequence 0x44: shop"},
    {0x45, "Sequence 0x45: Kaepora Gaebora"},
    {0x46, "Sequence 0x46: shooting gallery"},
    {0x47, "Sequence 0x47: Song of Soaring, ocarina"},
    {0x48, "Sequence 0x48: Song of Healing, ocarina"},
    {0x49, "Sequence 0x49: Inverted Song of Time"},
    {0x4A, "Sequence 0x4A: Song of Double Time"},
    {0x4B, "Sequence 0x4B: Sonata of Awakening"},
    {0x4C, "Sequence 0x4C: Goron Lullaby"},
    {0x4D, "Sequence 0x4D: New Wave Bossa Nova, first day"},
    {0x4E, "Sequence 0x4E: Elegy of Emptiness"},
    {0x4F, "Sequence 0x4F: Oath to Order"},
    {0x50, "Sequence 0x50: Swordsman's School"},
    {0x51, "Sequence 0x51: Goron Lullaby, introduction"},
    {0x52, "Sequence 0x52: new song learned"},
    {0x53, "Sequence 0x53: Bremen March"},
    {0x54, "Sequence 0x54: Ballad of the Wind Fish, first day"},
    {0x55, "Sequence 0x55: Song of Soaring"},
    {0x57, "Sequence 0x57: final hours and Majora's lair"},
    {0x58, "Sequence 0x58: Mikau's riff"},
    {0x59, "Sequence 0x59: Mikau's finale"},
    {0x5A, "Sequence 0x5A: Frog Song"},
    {0x5B, "Sequence 0x5B: Sonata of Awakening, ocarina"},
    {0x5C, "Sequence 0x5C: Goron Lullaby, ocarina"},
    {0x5D, "Sequence 0x5D: New Wave Bossa Nova, ocarina"},
    {0x5E, "Sequence 0x5E: Elegy of Emptiness, ocarina"},
    {0x5F, "Sequence 0x5F: Oath to Order, ocarina"},
    {0x62, "Sequence 0x62: guitar and bass session, ocarina"},
    {0x63, "Sequence 0x63: piano session"},
    {0x64, "Sequence 0x64: The Indigo-Go's session"},
    {0x65, "Sequence 0x65: Snowhead Temple"},
    {0x66, "Sequence 0x66: Great Bay Temple"},
    {0x67, "Sequence 0x67: New Wave Bossa Nova, saxophone"},
    {0x68, "Sequence 0x68: New Wave Bossa Nova, vocal"},
    {0x69, "Sequence 0x69: Majora's Wrath battle"},
    {0x6A, "Sequence 0x6A: Majora's Incarnation battle"},
    {0x6B, "Sequence 0x6B: Majora's Mask battle"},
    {0x6C, "Sequence 0x6C: bass session"},
    {0x6D, "Sequence 0x6D: drums session"},
    {0x6E, "Sequence 0x6E: piano session, part 2"},
    {0x6F, "Sequence 0x6F: Ikana Castle"},
    {0x70, "Sequence 0x70: the Giants gather"},
    {0x71, "Sequence 0x71: Kamaro's dance"},
    {0x72, "Sequence 0x72: Cremia's carriage"},
    {0x73, "Sequence 0x73: Keaton's quiz"},
    {0x74, "Sequence 0x74: staff roll, part 1"},
    {0x75, "Sequence 0x75: opening, loop"},
    {0x76, "Sequence 0x76: title screen"},
    {0x77, "Sequence 0x77: a dungeon appears"},
    {0x78, "Sequence 0x78: Woodfall cleared"},
    {0x79, "Sequence 0x79: Snowhead cleared"},
    {0x7B, "Sequence 0x7B: into the Moon"},
    {0x7C, "Sequence 0x7C: goodbye to the Giants"},
    {0x7D, "Sequence 0x7D: Tatl and Tael"},
    {0x7E, "Sequence 0x7E: the Moon's destruction"},
    {0x7F, "Sequence 0x7F: staff roll, part 2"},
};

// The patch that replaces the one word of the driver's code that holds `word`.
RdramPatch DriverWordPatch(const Driver& driver, uint32_t word, uint32_t replacement)
{
    const auto found = std::ranges::find(driver.code, word);
    if (found == driver.code.end() || std::find(found + 1, driver.code.end(), word) != driver.code.end())
    {
        throw std::logic_error("Majora's Mask's driver doesn't hold a word for a day call exactly once");
    }

    return {.address = driver.address + static_cast<uint32_t>(4 * (found - driver.code.begin())),
            .words = {replacement}};
}

// The driver's calls send days 1 and 2. These patches change the call for day 2 into a call for day 0, in the minis of
// the first-day tracks that require the command. A further call in the driver would change the library and every mini.
std::vector<RdramPatch> FirstDayPatches(const Driver& driver)
{
    constexpr uint32_t kDay2 = 2;

    return {DriverWordPatch(driver, mips::Addiu(mips::kT0, mips::kZero, kDay2 << kDayShift),
                            mips::Addiu(mips::kT0, mips::kZero, 0)),
            DriverWordPatch(driver, mips::Ori(mips::kA0, mips::kA0, kDay2), mips::Ori(mips::kA0, mips::kA0, 0))};
}

// The patches that make the driver's call for `day` queue `command` for every track whose number holds no day.
std::vector<RdramPatch> DayCallPatches(const Driver& driver, uint32_t day, uint32_t command)
{
    const RdramPatch match =
        DriverWordPatch(driver, mips::Addiu(mips::kT0, mips::kZero, static_cast<uint16_t>(day << kDayShift)),
                        mips::Addiu(mips::kT0, mips::kZero, 0));
    const RdramPatch lower = DriverWordPatch(driver, mips::Ori(mips::kA0, mips::kA0, static_cast<uint16_t>(day)),
                                             mips::Ori(mips::kA0, mips::kA0, command & 0xFFFF));

    // A LUI before the ORI loads the command's upper half.
    const uint32_t upper = lower.address - 4;
    if (driver.code[(upper - driver.address) / 4] != mips::Lui(mips::kA0, kSetDayPort >> 16))
    {
        throw std::logic_error("Majora's Mask's driver doesn't load the day command with a LUI and an ORI");
    }

    return {match, {.address = upper, .words = {mips::Lui(mips::kA0, command >> 16), lower.words[0]}}};
}

} // namespace

GameProfile NusNzseUsa()
{
    GameProfile profile;
    profile.game = "The Legend of Zelda: Majora's Mask";
    profile.serial = "NUS-NZSE-USA";
    profile.sha1 = "d6133ace5afaa0882cf214cf88daba39e266c078";
    profile.year = "2000";
    profile.cic = n64::Cic::k6105;
    profile.sound_driver = "Majora's Mask sound engine";
    profile.controller = true;
    profile.rdram_size = n64::kRdramSize8;

    profile.snapshot_address = kFrame;
    profile.snapshot_hit = 1;
    profile.quiet_tries = 1;
    profile.boot_seconds = 5.0;

    CallDriverOptions driver;
    driver.address = kDriverAddress;
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

    driver.frame_calls = {{.function = kAudioUpdate}};
    driver.fields = 3;
    profile.driver = BuildCallDriver(driver);

    profile.loop_watches.push_back(
        {.address = kSequenceJump, .target_register = 25, .match_register = 5, .match_value = kSequenceScriptState});

    for (const Title& title : kTitles)
    {
        Track track;
        track.number = title.number;
        track.name = title.name;
        if (std::ranges::find(kFirstDaySequences, title.number) != std::end(kFirstDaySequences))
        {
            track.rdram_patches = FirstDayPatches(*profile.driver);
        }
        profile.tracks.push_back(track);
    }

    for (const DayVersion& version : kDayVersions)
    {
        Track track;
        track.number = version.Number();
        track.name = version.name;
        profile.tracks.push_back(track);
    }

    for (const CommandVersion& version : kCommandVersions)
    {
        Track track;
        track.number = version.sequence;
        track.name = version.name;
        for (uint32_t day = 1; day <= 2; day++)
        {
            const uint32_t command = version.commands[day - 1];
            if (command != 0)
            {
                const std::vector<RdramPatch> patches = DayCallPatches(*profile.driver, day, command);
                track.rdram_patches.insert(track.rdram_patches.end(), patches.begin(), patches.end());
            }
        }
        profile.tracks.push_back(track);
    }

    for (const uint32_t sequence : kSongFanfares)
    {
        const auto title = std::ranges::find(kTitles, sequence, &Title::number);
        if (title == std::end(kTitles))
        {
            throw std::logic_error("a song's fanfare in Majora's Mask has no title");
        }

        for (const Form& form : kForms)
        {
            Track track;
            track.number = sequence;
            track.name = std::string(title->name) + form.name;
            track.rdram_patches = DayCallPatches(*profile.driver, 1, kSetPort7 | form.value);
            profile.tracks.push_back(track);
        }
    }

    return profile;
}

} // namespace usf::rip::games
