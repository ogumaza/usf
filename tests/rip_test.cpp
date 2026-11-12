// SPDX-License-Identifier: MIT

// The ripper's machinery on a small program made with the MIPS encoder. The program reads its track number from a0,
// copies 256 bytes of the ROM that the number picks into RDRAM by PI DMA, reads a gain from a table in RDRAM and two
// words that every track shares, and then plays buffers made from those bytes for ever. Its tracks therefore read some
// words that every track reads and some that only they read.
//
// The checks: coverage finds those words; a set trimmed by it puts the whole state and the shared ROM words in the
// library, and each track's other ROM words and its selector in its mini; each mini, written and loaded back, renders
// the same samples as the untrimmed state (the anchor: a render of the untrimmed state through the same core but not
// through the files); a mini that lacks its ROM data renders differently; a track that needs no run doesn't run; a loop
// watch gives the length analysis its loops, from one jump back a pass or from one for each track of a pass; and
// StopThread unlinks libultra threads.

#include "rip/rip.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "xsflib/bytes.h"
#include "xsflib/cli/rip.h"
#include "xsflib/files.h"
#include "xsflib/rip/file_names.h"
#include "xsflib/testing/check.h"
#include "xsflib/testing/temp_folder.h"

#include "n64/save_state.h"
#include "n64/system.h"
#include "rip/mips.h"
#include "rip/profiles.h"
#include "rip/snapshot.h"
#include "rip/trim.h"
#include "test_mips.h"
#include "test_programs.h"
#include "usf/usf_file.h"

namespace
{

namespace mips = usf::rip::mips;

using mips::kA0;
using mips::kS0;
using mips::kS1;
using mips::kS2;
using mips::kT0;
using mips::kT1;
using mips::kT2;
using mips::kT3;
using mips::kT4;
using mips::kT5;
using mips::kT6;
using mips::kT7;
using mips::kT8;
using mips::kT9;
using mips::kZero;
using usf::n64::kKseg0;
using usf::n64::kStateRdramOffset;
using usf::testing::kCode;
using usf::testing::kStateA0;

// The program's memory, by physical address.
constexpr uint32_t kSequence = 0x00100000; // Where the track's ROM bytes go.
constexpr uint32_t kGains = 0x00180000;    // One word for each track.
constexpr uint32_t kShared = 0x00181000;   // A word that every track reads.
constexpr uint32_t kBuffer = 0x00200000;

// The ROM's layout: each track's 256 bytes, and a word that every track reads.
constexpr uint32_t kTrackData = 0x1000;
constexpr uint32_t kSharedRom = 0x4000;

constexpr int kTracks = 3;

// The program, the address of its jump back to the start of its loop, and the address of the tracks' jumps back.
struct Program
{
    std::vector<uint32_t> words;
    uint32_t loop_jump = 0;
    uint32_t track_jump = 0;
};

// Copy the track's 256 bytes from the ROM into RDRAM, and wait for the DMA.
void EmitTrackCopy(mips::Assembler& code)
{
    code.Emit(mips::Lui(kT0, 0xA460));
    code.Emit(mips::Lui(kT1, kSequence >> 16));
    code.Emit(mips::Sw(kT1, kT0, 0x0));
    code.Emit(mips::Sll(kT2, kA0, 8));
    code.LoadImmediate(kT3, 0x10000000 | kTrackData);
    code.Emit(mips::Addu(kT3, kT3, kT2));
    code.Emit(mips::Sw(kT3, kT0, 0x4));
    code.Emit(mips::Addiu(kT4, kZero, 0xFF));
    code.Emit(mips::Sw(kT4, kT0, 0xC));
    const uint32_t wait_pi = code.Here();
    code.Emit(mips::Lw(kT5, kT0, 0x10));
    code.Emit(mips::Andi(kT5, kT5, 3));
    code.Bne(kT5, kZero, wait_pi);
    code.Emit(mips::kNop);
}

// The gain: the track's entry in the table, the shared RDRAM word and the shared ROM word.
void EmitGain(mips::Assembler& code)
{
    code.Emit(mips::Lui(kT6, mips::High(kKseg0 | kGains)));
    code.Emit(mips::Sll(kT7, kA0, 2));
    code.Emit(mips::Addu(kT6, kT6, kT7));
    code.Emit(mips::Lw(kS1, kT6, 0));
    code.Emit(mips::Lui(kT6, mips::High(kKseg0 | kShared)));
    code.Emit(mips::Lw(kT7, kT6, mips::Low(kKseg0 | kShared)));
    code.Emit(mips::Addu(kS1, kS1, kT7));
    code.Emit(mips::Lui(kT6, 0xB000));
    code.Emit(mips::Lw(kT7, kT6, kSharedRom));
    code.Emit(mips::Addu(kS1, kS1, kT7));
}

// Each buffer: 256 frames, frame i from byte (counter + i) mod 256 plus the gain.
void EmitFill(mips::Assembler& code)
{
    code.Emit(mips::Lui(kT0, mips::High(kKseg0 | kBuffer)));
    code.Emit(mips::Addiu(kT1, kZero, 0));
    const uint32_t fill = code.Here();
    code.Emit(mips::Addu(kT2, kS0, kT1));
    code.Emit(mips::Andi(kT2, kT2, 0xFF));
    code.Emit(mips::Lui(kT3, mips::High(kKseg0 | kSequence)));
    code.Emit(mips::Addu(kT3, kT3, kT2));
    code.Emit(mips::Lbu(kT4, kT3, 0));
    code.Emit(mips::Addu(kT4, kT4, kS1));
    code.Emit(mips::Andi(kT4, kT4, 0xFF));
    code.Emit(mips::Sll(kT5, kT4, 6));
    code.Emit(mips::Sll(kT6, kT4, 22));
    code.Emit(mips::Or(kT5, kT5, kT6));
    code.Emit(mips::Sll(kT6, kT1, 2));
    code.Emit(mips::Addu(kT6, kT6, kT0));
    code.Emit(mips::Sw(kT5, kT6, 0));
    code.Emit(mips::Addiu(kT1, kT1, 1));
    code.Emit(mips::Addiu(kT7, kZero, 256));
    code.Bne(kT1, kT7, fill);
    code.Emit(mips::kNop);
}

// Hand the buffer to the AI, and wait while the AI is full.
void EmitHandOff(mips::Assembler& code)
{
    usf::testing::EmitAiBuffer(code, kBuffer >> 16, 1024);
    code.Emit(mips::Addiu(kS0, kS0, 7));
    usf::testing::EmitAiWait(code, 31);
}

// The jumps back of `loop_tracks` tracks, one after another (see MakeProgram). Returns the address of the jump.
uint32_t EmitTrackJumps(mips::Assembler& code, int loop_tracks, bool track_addresses)
{
    code.Emit(mips::Addiu(kT3, kZero, 0));
    const uint32_t next_track = code.Here();
    code.Emit(mips::Sll(kT9, kT3, 4));
    if (track_addresses)
    {
        code.Emit(mips::Lui(kT7, 0x8000));
        code.Emit(mips::Or(kT7, kT7, kT9));
    }

    const uint32_t track_jump = code.Here();
    code.Emit(mips::Addiu(kT3, kT3, 1));
    code.Emit(mips::Addiu(kT8, kZero, static_cast<uint16_t>(loop_tracks)));
    code.Bne(kT3, kT8, next_track);
    code.Emit(mips::kNop);

    return track_jump;
}

// With `loop_tracks`, the end of each pass also takes a jump back for each of that many tracks, one after another, as a
// sequence player whose tracks loop separately does. At `track_jump`, t3 holds the track and t9 its loop's target. With
// `track_addresses`, t7 also holds an address in KSEG0 for the track there, as a player that names a channel by its
// address does.
Program MakeProgram(int loop_tracks = 0, bool track_addresses = false)
{
    mips::Assembler code(kKseg0 | kCode);
    EmitTrackCopy(code);
    EmitGain(code);

    // The AI at 48,681,812 / 1,521 Hz, about 32 kHz.
    code.Emit(mips::Addiu(kS0, kZero, 0));
    usf::testing::EmitAiSetup(code, mips::Addiu(kT0, kZero, 1520));

    const uint32_t loop = code.Here();
    EmitFill(code);
    EmitHandOff(code);

    Program program;
    if (loop_tracks > 0)
    {
        program.track_jump = EmitTrackJumps(code, loop_tracks, track_addresses);
    }

    program.loop_jump = code.Here();
    code.J(loop);
    code.Emit(mips::kNop);
    program.words = code.Words();

    return program;
}

std::vector<uint8_t> MakeRom()
{
    std::vector<uint8_t> rom = usf::testing::TestRom(0x8000);
    for (uint32_t i = 0; i < kTracks * 0x100; i++)
    {
        rom[kTrackData + i] = static_cast<uint8_t>((i * 37 + i / 0x100 * 101) & 0xFF);
    }

    xsflib::StoreBe32(rom.data() + kSharedRom, 11);

    return rom;
}

// The snapshot: the program about to start, with its track number in a0.
usf::rip::Snapshot MakeSnapshot(const Program& program)
{
    usf::n64::System system(usf::testing::LegacyOptions());
    system.LoadRom(MakeRom());
    usf::testing::StartProgram(system, program.words, 0);
    for (uint32_t track = 0; track < kTracks; track++)
    {
        system.WritePhysical32(kGains + 4 * track, 3 + 2 * track);
    }
    system.WritePhysical32(kShared, 5);

    usf::rip::Snapshot snapshot;
    snapshot.rom = system.Rom();
    snapshot.state = system.SaveState();
    snapshot.selector_kind = usf::rip::TrackSelector::Kind::kRegister;
    snapshot.selector_offset = kStateA0;
    snapshot.selector_size = 8;

    return snapshot;
}

usf::rip::GameProfile MakeProfile()
{
    usf::rip::GameProfile profile;
    profile.game = "Test";
    for (uint32_t track = 0; track < kTracks; track++)
    {
        usf::rip::Track entry;
        entry.number = track;
        entry.name = "Track " + std::to_string(track);
        entry.length_seconds = 2.0;
        profile.tracks.push_back(entry);
    }

    return profile;
}

// The program's tracks, each run with coverage and a reference render.
struct TrackRuns
{
    usf::rip::Snapshot snapshot;
    usf::rip::GameProfile profile;
    std::vector<usf::rip::TrackRun> runs;
    std::vector<usf::rip::TrackWords> words;
};

TrackRuns RunTracks()
{
    TrackRuns tracks;
    tracks.snapshot = MakeSnapshot(MakeProgram());
    tracks.profile = MakeProfile();
    usf::rip::RunSettings settings;
    settings.lengths = false;
    settings.margin_seconds = 1.5;
    for (const usf::rip::Track& track : tracks.profile.tracks)
    {
        tracks.runs.push_back(usf::rip::RunTrack(tracks.profile, tracks.snapshot, track, settings));
        tracks.words.push_back(tracks.runs.back().words);
    }

    return tracks;
}

// Coverage: each track read the bytes of its number, the shared words and the code, but no other track's bytes, and
// none of the RDRAM that the DMA or the program wrote before reading it.
void Coverage(const TrackRuns& tracks)
{
    for (uint32_t track = 0; track < kTracks; track++)
    {
        const usf::rip::TrackWords& read = tracks.words[track];
        for (uint32_t other = 0; other < kTracks; other++)
        {
            XSFLIB_CHECK(read.rom.Has((kTrackData + 0x100 * other) / 4) == (other == track));
            XSFLIB_CHECK(read.rdram.Has((kGains + 4 * other) / 4) == (other == track));
        }
        XSFLIB_CHECK(read.rom.Has(kSharedRom / 4));
        XSFLIB_CHECK(read.rdram.Has(kShared / 4) && read.rdram.Has(kCode / 4));
        XSFLIB_CHECK(!read.rdram.Has(kSequence / 4) && !read.rdram.Has(kBuffer / 4));
    }

    // The tracks differ, so the selector reaches the program.
    XSFLIB_CHECK(tracks.runs[0].reference.size() == 2 && tracks.runs[0].reference != tracks.runs[1].reference);
}

// The set: the library holds the whole state and the shared ROM word. Each mini holds its track's ROM bytes and its
// number in a0, the selector at state offset 0x070.
void SetLayout(const TrackRuns& tracks)
{
    const usf::rip::SetSections set = usf::rip::BuildSet(tracks.snapshot, tracks.profile.tracks, &tracks.words);

    XSFLIB_REQUIRE(set.library.rom && set.library.state && set.minis.size() == kTracks);
    XSFLIB_CHECK(set.library.rom->size() == 1 && set.library.rom->front().offset == kSharedRom);
    XSFLIB_CHECK(set.library.state->size() == 1);
    XSFLIB_CHECK(set.library.state->front().offset == 0);
    XSFLIB_CHECK(set.library.state->front().data == tracks.snapshot.state);
    XSFLIB_REQUIRE(set.minis[1].rom);
    XSFLIB_CHECK(set.minis[1].rom->size() == 1);
    XSFLIB_CHECK(set.minis[1].rom->front().offset == kTrackData + 0x100);
    for (uint32_t track = 0; track < kTracks; track++)
    {
        const usf::Sections& mini = set.minis[track];
        XSFLIB_REQUIRE(mini.state && mini.state->size() == 1);
        const usf::Chunk& selector = mini.state->front();
        XSFLIB_REQUIRE(selector.data.size() == 8);
        XSFLIB_CHECK(selector.offset == 0x070);
        XSFLIB_CHECK(xsflib::LoadLe64(selector.data.data()) == track);
    }
}

// A mini to write: the number in its file's name, its title and its timing. usfrip passes the same fields to xsflib's
// WriteMinis.
struct MiniJob
{
    uint32_t index = 0;
    std::string label;
    std::optional<xsflib::rip::Timing> timing;
};

// The set's files: each mini, written and loaded back with its library, renders the same samples as the untrimmed
// state does.
void WrittenFiles(const TrackRuns& tracks)
{
    const usf::rip::GameProfile& profile = tracks.profile;
    const usf::rip::SetSections set = usf::rip::BuildSet(tracks.snapshot, profile.tracks, &tracks.words);
    XSFLIB_REQUIRE(set.minis.size() == kTracks);
    // A mini without its ROM data plays the wrong bytes.
    usf::Sections broken = set.minis[2];
    broken.rom.reset();
    xsflib::testing::TempFolder folder("usf_rip_test");
    usf::rip::SetTags tags;
    tags.ripper = "usfrip test";

    const std::vector<uint8_t> library = usf::rip::LibraryFile(profile, set.library, tags);
    XSFLIB_CHECK(!xsflib::WriteFile(folder.Path() / "Test.usflib", library));

    // The minis, written as usfrip writes them.
    std::vector<MiniJob> jobs;
    for (size_t i = 0; i < profile.tracks.size(); i++)
    {
        jobs.push_back({static_cast<uint32_t>(i + 1), profile.tracks[i].name, tracks.runs[i].timing});
    }

    // The broken mini gets the tags of the third track's mini.
    xsflib::psf::Tags broken_tags;
    const auto build = [&](const MiniJob& job, const xsflib::psf::Tags& mini_tags)
    {
        if (job.index == 3)
        {
            broken_tags = mini_tags;
        }

        return usf::rip::MiniFile(profile, set.minis[job.index - 1], job.index, "Test.usflib", tags.ripper, mini_tags);
    };
    xsflib::rip::FileNames names(".usflib", ".miniusf");
    std::vector<std::filesystem::path> paths;
    xsflib::cli::MiniSettings settings;
    settings.paths = &paths;
    settings.stream = stdout;
    XSFLIB_REQUIRE(xsflib::cli::WriteMinis(folder.Path(), names, jobs, tags.common, settings, build) == kTracks);

    const std::filesystem::path broken_path = folder.Path() / "broken.miniusf";
    XSFLIB_CHECK(!xsflib::WriteFile(broken_path,
                                    usf::rip::MiniFile(profile, broken, 3, "Test.usflib", tags.ripper, broken_tags)));
    usf::LoadedUsf loaded;
    const auto load_error = usf::LoadUsf(xsflib::Utf8(paths[1]), xsflib::psf::ReadWholeFile, loaded);

    // The anchor: the untrimmed state's render.
    for (size_t i = 0; i < paths.size(); i++)
    {
        XSFLIB_CHECK(!usf::rip::CheckTrack(xsflib::Utf8(paths[i]), tracks.runs[i]));
    }
    XSFLIB_CHECK(usf::rip::CheckTrack(xsflib::Utf8(broken_path), tracks.runs[2]).has_value());
    // The mini's tags, and a library that keeps a small part of the ROM.
    XSFLIB_CHECK(!load_error);
    XSFLIB_CHECK(loaded.enable_compare && loaded.enable_fifo_full);
    XSFLIB_CHECK(loaded.tags["title"] == "Track 1" && loaded.tags["length"] == "0:02.000");
    XSFLIB_CHECK(loaded.tags["usfrip"] == "usfrip test" && loaded.tags["_lib"] == "Test.usflib");
    XSFLIB_CHECK(loaded.rom.size() == kSharedRom + 4);
}

// A track with no length analysis, no coverage and no reference to make doesn't run. Its state here would make the core
// throw: its identifier is zero.
void NoRun()
{
    usf::rip::Snapshot snapshot;
    snapshot.state.assign(usf::kStateOverhead + usf::n64::kRdramSize4, 0);
    snapshot.selector_offset = kStateA0;
    snapshot.selector_size = 8;
    const usf::rip::GameProfile profile = MakeProfile();
    usf::rip::Track track = profile.tracks[0];
    track.length_seconds.reset();
    usf::rip::RunSettings settings;
    settings.lengths = false;
    settings.coverage = false;
    settings.reference = false;

    const usf::rip::TrackRun run = usf::rip::RunTrack(profile, snapshot, track, settings);

    XSFLIB_CHECK(!run.timing && run.reference.empty() && run.check_frames == 0);
}

// Runs track 0 of `program` without coverage, with `watch` as the profile's one loop watch, and checks the length of
// two or three buffers and the default fade that the analysis gives.
void CheckLoops(const Program& program, const usf::rip::LoopWatch& watch)
{
    const usf::rip::Snapshot snapshot = MakeSnapshot(program);
    usf::rip::GameProfile profile = MakeProfile();
    profile.tracks[0].length_seconds.reset();
    profile.loop_watches.push_back(watch);
    usf::rip::RunSettings settings;
    settings.coverage = false;
    settings.margin_seconds = 0.5;

    const usf::rip::TrackRun run = usf::rip::RunTrack(profile, snapshot, profile.tracks[0], settings);

    XSFLIB_REQUIRE(run.timing.has_value());
    XSFLIB_CHECK(run.timing->length_ms >= 15 && run.timing->length_ms <= 24);
    XSFLIB_CHECK(run.timing->fade_ms == 10000 && !run.timing->silent);
}

// A loop watch at the jump back to the start of the program's loop: each buffer is a pass of the loop. The analysis
// counts two passes after the first sound and fades from there: a length of two or three buffers of 256 frames at
// 32,006 Hz, 8 ms each, and the default fade of 10 s.
void LoopWatch()
{
    const Program program = MakeProgram();

    CheckLoops(program, {kKseg0 | program.loop_jump, -1, -1});
}

// A loop watch at the tracks' jumps back, with the track in t3 and the target in t9: each buffer is a pass in which
// three tracks loop. The analysis counts the passes of track 0 and fades once each other track has looped twice too:
// two or three buffers, as with one jump back a pass.
void TrackLoops()
{
    const Program program = MakeProgram(3);

    CheckLoops(program, {kKseg0 | program.track_jump, mips::kT9, mips::kT3});
}

// The same, with each track named by its address in t7. An address in KSEG0 converts to a negative track number. The
// analysis still counts the passes of the first track that loops.
void TrackAddresses()
{
    const Program program = MakeProgram(3, true);

    CheckLoops(program, {kKseg0 | program.track_jump, mips::kT9, mips::kT7});
}

// A queue of three waiting threads at kThreadQueue. The queue ends at kQueueTail.
constexpr uint32_t kThreadQueue = 0x00190000;
constexpr uint32_t kThreads[] = {0x00190100, 0x00190200, 0x00190300};
constexpr uint32_t kQueueTail = 0x00190400;

// libultra's OSThread: next at 0x00, priority at 0x04, queue at 0x08, state (16 bits) at 0x10. A queue ends at a thread
// of priority -1.
void WriteThreadQueue(usf::n64::System& system)
{
    system.GetCpu().SetCop0(usf::n64::kCop0Status, 0);
    system.WritePhysical32(kThreadQueue, kKseg0 | kThreads[0]);
    for (int i = 0; i < 3; i++)
    {
        system.WritePhysical32(kThreads[i], kKseg0 | (i < 2 ? kThreads[i + 1] : kQueueTail));
        system.WritePhysical32(kThreads[i] + 4, 10);
        system.WritePhysical32(kThreads[i] + 8, kKseg0 | kThreadQueue);
        system.WritePhysical32(kThreads[i] + 0x10, 0x00080000); // Waiting.
    }
    system.WritePhysical32(kQueueTail + 4, 0xFFFFFFFF);
}

void StopThreads()
{
    usf::n64::System system;
    WriteThreadQueue(system);

    usf::rip::StopThread(system, kKseg0 | kThreads[1]);
    usf::rip::StopThread(system, kKseg0 | kThreads[0]);

    XSFLIB_CHECK(system.ReadPhysical32(kThreadQueue) == (kKseg0 | kThreads[2]));
    XSFLIB_CHECK(system.ReadPhysical32(kThreads[0] + 0x10) >> 16 == 1);
    XSFLIB_CHECK(system.ReadPhysical32(kThreads[1] + 0x10) >> 16 == 1);
    XSFLIB_CHECK(system.ReadPhysical32(kThreads[1] + 8) == 0);
    XSFLIB_CHECK(system.ReadPhysical32(kThreads[2]) == (kKseg0 | kQueueTail));
}

// A running thread can't be stopped.
void RunningThread()
{
    usf::n64::System system;
    WriteThreadQueue(system);
    system.WritePhysical32(kThreads[2] + 0x10, 0x00040000); // Running.
    const auto stop = [&system]
    {
        usf::rip::StopThread(system, kKseg0 | kThreads[2]);
    };

    const std::string error = xsflib::testing::ErrorOf<std::runtime_error>(stop);

    XSFLIB_CHECK(error.find("is running") != std::string::npos);
}

// A blank snapshot whose selector is `value_size` bytes at `address` in RDRAM. The selector word is at the state offset
// of RDRAM address 0x1000.
usf::rip::Snapshot RdramSelectorSnapshot(uint32_t address, uint32_t value_size)
{
    usf::rip::Snapshot snapshot;
    snapshot.state.assign(usf::kStateOverhead + usf::n64::kRdramSize4, 0);
    snapshot.selector_kind = usf::rip::TrackSelector::Kind::kRdram;
    snapshot.selector_address = address;
    snapshot.selector_value_size = value_size;
    snapshot.selector_offset = kStateRdramOffset + 0x1000;
    snapshot.selector_size = 4;

    return snapshot;
}

// The selector in RDRAM: a mini writes the number into its bytes, in LE word order.
void RdramSelector()
{
    const usf::rip::Snapshot snapshot = RdramSelectorSnapshot(0x1002, 2);

    const std::vector<uint8_t> state = usf::rip::TrackState(snapshot, 0xABCD);

    // Console bytes 0x1002 and 0x1003 hold 0xAB and 0xCD, at state positions 0x75C + (A XOR 3).
    XSFLIB_CHECK(state[kStateRdramOffset + (0x1002 ^ 3)] == 0xAB && state[kStateRdramOffset + (0x1003 ^ 3)] == 0xCD);
    XSFLIB_CHECK(xsflib::LoadLe32(state.data() + kStateRdramOffset + 0x1000) == 0x0000ABCD);
}

// A track's patches: the mini holds them beside the selector, and they replace what the snapshot holds.
void TrackPatches()
{
    usf::rip::Snapshot snapshot = RdramSelectorSnapshot(0x1000, 4);
    xsflib::StoreLe32(snapshot.state.data() + kStateRdramOffset + 0x2004, 0x11111111);
    usf::rip::Track track;
    track.number = 7;
    track.rdram_patches = {{.address = 0x80002000, .words = {0x01020304, 0x05060708}},
                           {.address = 0xA0003000, .words = {0x090A0B0C}}};

    const std::vector<uint8_t> state = usf::rip::TrackState(snapshot, track);
    const usf::rip::SetSections set = usf::rip::BuildSet(snapshot, {track}, nullptr);

    XSFLIB_CHECK(xsflib::LoadLe32(state.data() + kStateRdramOffset + 0x1000) == 7);
    XSFLIB_CHECK(xsflib::LoadLe32(state.data() + kStateRdramOffset + 0x2000) == 0x01020304);
    XSFLIB_CHECK(xsflib::LoadLe32(state.data() + kStateRdramOffset + 0x2004) == 0x05060708);
    XSFLIB_CHECK(xsflib::LoadLe32(state.data() + kStateRdramOffset + 0x3000) == 0x090A0B0C);

    // The console's bytes, in LE word order.
    XSFLIB_CHECK(state[kStateRdramOffset + (0x2000 ^ 3)] == 0x01 && state[kStateRdramOffset + (0x2003 ^ 3)] == 0x04);

    XSFLIB_REQUIRE(set.minis.size() == 1 && set.minis[0].state && set.minis[0].state->size() == 3);
    const std::vector<usf::Chunk>& chunks = *set.minis[0].state;
    XSFLIB_CHECK(chunks[0].offset == kStateRdramOffset + 0x1000);
    XSFLIB_CHECK(chunks[1].offset == kStateRdramOffset + 0x2000);
    XSFLIB_CHECK(chunks[1].data.size() == 8);
    XSFLIB_CHECK(chunks[2].offset == kStateRdramOffset + 0x3000);
}

// A patch's address must be in KSEG0 or KSEG1. The patch must end within the 4 MiB of RDRAM. After the RDRAM, the
// state holds DMEM.
void RefusedPatches()
{
    const usf::rip::Snapshot snapshot = RdramSelectorSnapshot(0x1000, 4);
    const auto patch = [&snapshot](uint32_t address, const std::vector<uint32_t>& words)
    {
        usf::rip::Track bad;
        bad.rdram_patches = {{.address = address, .words = words}};
        return xsflib::testing::ErrorOf<std::runtime_error>([&] { (void)usf::rip::TrackState(snapshot, bad); });
    };

    const std::string physical = patch(0x00002000, {1});
    const std::string past_rdram = patch(0x803FFFFC, {1, 2});

    XSFLIB_CHECK(physical.find("KSEG0 or KSEG1") != std::string::npos);
    XSFLIB_CHECK(past_rdram.find("runs past RDRAM") != std::string::npos);
}

// An image with more than one set names each set, and no two of its sets alike.
void ImageSets()
{
    std::set<std::string> images;
    for (const usf::rip::GameProfile& profile : usf::rip::Profiles())
    {
        images.insert(profile.sha1);
    }

    for (const std::string& sha1 : images)
    {
        const std::vector<const usf::rip::GameProfile*> found = usf::rip::FindProfiles(sha1);
        XSFLIB_CHECK(!found.empty());
        if (found.size() > 1)
        {
            std::set<std::string> names;
            for (const usf::rip::GameProfile* profile : found)
            {
                XSFLIB_CHECK(!profile->set.empty() && names.insert(profile->set).second);
            }
        }
    }

    // Super Mario 64 (USA): the music first, then the sound effects, which --bgm leaves out.
    const std::vector<const usf::rip::GameProfile*> sm64 =
        usf::rip::FindProfiles("9bef1128717f958171a4afac3ed78ee2bb4e86ce");
    XSFLIB_CHECK(sm64.size() == 2 && !sm64[0]->sound_effects && sm64[1]->sound_effects);
    XSFLIB_CHECK(usf::rip::FindProfiles("0000000000000000000000000000000000000000").empty());
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("rip",
                                    []
                                    {
                                        const TrackRuns tracks = RunTracks();
                                        Coverage(tracks);
                                        SetLayout(tracks);
                                        WrittenFiles(tracks);
                                        NoRun();
                                        LoopWatch();
                                        TrackLoops();
                                        TrackAddresses();
                                        StopThreads();
                                        RunningThread();
                                        RdramSelector();
                                        TrackPatches();
                                        RefusedPatches();
                                        ImageSets();
                                    });
}
