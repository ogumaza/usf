// SPDX-License-Identifier: MIT

// A game profile: what the ripper needs to know about a game's code to rip its music. Each supported game has one.
//
// The ripper boots the game and runs it to the profile's snapshot point. It then applies the profile's patches, stops
// the threads the profile names, installs the profile's driver if it has one, and checks that the moment is quiet. It
// saves the state there. Each track's mini writes the track's number into the selector: a CPU register or a value in
// RDRAM. Playback then starts at the snapshot with that number in place.
//
// Addresses are virtual addresses as the game's code uses them (KSEG0, 0x80000000 and up) unless a field says
// otherwise. Values in RDRAM are in the console's byte order (big-endian).
//
// Fields are only ever added to this struct, never changed or removed.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "xsflib/rip/rom_set.h"

#include "n64/constants.h"
#include "n64/pif.h"
#include "n64/system.h"

namespace usf::rip
{

// Where the number that selects a track goes in the state.
struct TrackSelector
{
    enum class Kind
    {
        // A CPU register, r1 to r31. The mini sets the whole 64-bit register to the number, sign-extended from 32 bits.
        kRegister,

        // A value of `size` bytes in RDRAM at `address`, aligned to its size.
        kRdram,
    };

    Kind kind = Kind::kRegister;
    int reg = 4;          // For kRegister: the register's number. 4 is a0, the first argument of a call.
    uint32_t address = 0; // For kRdram.
    int size = 4;         // For kRdram: 1, 2 or 4 bytes.
};

// Words written into RDRAM.
struct RdramPatch
{
    uint32_t address = 0; // Word-aligned.
    std::vector<uint32_t> words;
};

// One track of the set: one mini.
struct Track
{
    uint32_t number = 0; // The value that the selector receives.
    std::string name;    // The title tag. The mini's file name comes from it too.

    // Override the length analysis. A track with a length and no fade gets no fade.
    std::optional<double> length_seconds;
    std::optional<double> fade_seconds;

    // Words that this track's mini writes into RDRAM beside the selector, for a track that a number alone can't select.
    // They replace what the snapshot holds there, the profile's patches included. The addresses are in KSEG0 or KSEG1.
    std::vector<RdramPatch> rdram_patches;
};

// Bytes changed in the ROM before boot. They reach the library's ROM section like the rest of the ROM data.
struct RomPatch
{
    uint32_t offset = 0; // A cartridge offset: 0 is cartridge address 0x10000000.
    std::vector<uint8_t> bytes;
};

// A driver: code that the ripper writes into RDRAM at the snapshot. The thread that reached the snapshot point runs it
// in place of the game's code, from its first word, with the registers and stack it had there. The driver reads the
// track's number from `track_word` and never returns. driver.h builds a generic one that calls game functions.
struct Driver
{
    uint32_t address = 0;       // Where the code goes. Word-aligned.
    std::vector<uint32_t> code; // Instruction words.
    uint32_t track_word = 0;    // The word that each mini sets to its track's number. It replaces the selector.
};

// A point in the game's sequence player that the length analysis watches: an instruction that runs each time a sequence
// jumps back to its loop. The analysis counts the loops there and fades a track out after its second loop. Where each
// of a sequence's tracks jumps back separately, a track register tells them apart. The analysis then counts the loops
// of the first track that loops after the first sound. It fades once every track that loops has looped twice.
struct LoopWatch
{
    uint32_t address = 0;

    // Registers that tell loops apart at that instruction, or -1. The loop's target address tells one loop from
    // another. The track or channel tells one part of the sequence from another.
    int target_register = -1;
    int track_register = -1;

    // A register that holds the loop's repeat count at that instruction, or -1. With it, a jump counts only when the
    // loop repeats for ever, with a count of 0xFF. A loop that repeats a set number of times doesn't show the end of a
    // song. It can be a short figure inside the song.
    int count_register = -1;

    // A register that must hold `match_value` at that instruction, or -1. With it, a jump counts only for that value.
    // An engine that runs the jumps of all its scripts through one function needs it to pick out the sequence's script.
    int match_register = -1;
    uint32_t match_value = 0;
};

// One chip of an arcade board's ROM set: its SHA-1 and size, where it goes in the image, and whether each pair of its
// bytes is stored in reverse, as on the Aleck64.
using RomChip = xsflib::rip::RomChip;

struct GameProfile
{
    // The game. `game` becomes the game tag and the set's name. `serial` names the image in messages, by its serial,
    // region and revision, such as "NUS-NCTE-USA-rev1". `sha1` is the SHA-1 of the image in big-endian byte order,
    // in lower-case hex. The ripper refuses an image with another hash.
    std::string game;
    std::string serial;
    std::string sha1;

    // An image may have more than one set, such as its music and its sound effects. Each set has a profile of its own,
    // with the image's SHA-1. `set` names the set, such as "music". When an image has more than one set, each set goes
    // in a folder of that name. `sound_effects` marks a set of sound effects, which usfrip's --bgm leaves out.
    std::string set;
    bool sound_effects = false;

    // Metadata for the tags. Empty fields are left out.
    std::string artist;
    std::string year;
    std::string copyright;

    // The boot chip, or kUnknown to let the boot find it. A CIC-NUS-6105 game needs a dump of the PIF ROM.
    n64::Cic cic = n64::Cic::kUnknown;
    uint32_t rdram_size = n64::kRdramSize4;

    // Changes to the ROM, made before boot.
    std::vector<RomPatch> rom_patches;

    // A controller is connected to port 0 (the first port) from boot until the snapshot. The rip never depends on one
    // after the snapshot: the existing players report every port empty. `input` below scripts it.
    bool controller = false;

    // The snapshot point: the instruction at `snapshot_address`, the `snapshot_hit`th time the CPU reaches it, within
    // `boot_seconds` of emulated time from boot. Often the call of the song-select function, with the song's number in
    // an argument register. If that moment isn't quiet, the ripper waits for the next arrival, up to `quiet_tries`
    // more times.
    uint32_t snapshot_address = 0;
    int snapshot_hit = 1;
    int quiet_tries = 0;
    double boot_seconds = 60.0;

    // Where each mini writes its track's number. A profile with a driver uses the driver's track word instead.
    TrackSelector selector;

    // Words written into RDRAM at the snapshot, before the state is saved.
    std::vector<RdramPatch> rdram_patches;

    // Threads that the ripper stops at the snapshot: the addresses of their OSThread structures. A thread stopped
    // this way is taken off the queue it waits in and marked stopped, as osStopThread does. The thread that is
    // running at the snapshot can't be stopped this way. A driver can take it over instead.
    std::vector<uint32_t> stop_threads;

    // A driver to install at the snapshot, if the game needs one.
    std::optional<Driver> driver;

    std::vector<Track> tracks;

    // The two flags. Together they give the most realistic timing in every legacy player. Turn one off only when the
    // game's code fails with it, and say why in a comment.
    bool enable_compare = true;
    bool enable_fifo_full = true;

    // The length analysis's loop points. Without any, a track ends when it has been silent for the analysis's
    // silence, or at its limit.
    std::vector<LoopWatch> loop_watches;

    // The name of the game's sound driver for the informational tag usfdriver, such as "libultra compressed sequence
    // player". Empty leaves the tag out.
    std::string sound_driver;

    // Input from boot until the snapshot: the script of controller states (pif.h), and the save chip. The ripper
    // connects a controller to each port that an event names, and to port 0 when `controller` is set. Neither
    // reaches the rip: the existing players report every port empty and emulate no save chip.
    std::vector<n64::InputEvent> input;
    n64::EepromType eeprom = n64::EepromType::kNone;

    // A game of an arcade board comes as the board's ROM set: chips that the ripper finds by their SHA-1 and size, in
    // a ZIP archive or a folder that the user gives, and puts together into the image. `sha1` is still the image's.
    std::vector<RomChip> chips;

    // The board, and its inputs from boot until the snapshot. Like the controllers, they don't reach the rip: the
    // existing players emulate a console, with nothing at the board's addresses.
    n64::Board board = n64::Board::kConsole;
    n64::BoardInputs board_inputs;
    std::vector<n64::BoardInputEvent> board_input;
};

} // namespace usf::rip
