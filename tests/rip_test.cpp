// SPDX-License-Identifier: MIT

// The ripper's machinery on a small program made with the MIPS encoder. The program reads its track number from a0,
// copies 256 bytes of the ROM that the number picks into RDRAM by PI DMA, reads a gain from a table in RDRAM and two
// words that every track shares, and then plays buffers made from those bytes for ever. Its tracks therefore read
// some words that every track reads and some that only they read.
//
// The checks: coverage finds those words; a set trimmed by it puts the whole state and the shared ROM words in the
// library, and each track's other ROM words and its selector in its mini; each mini, written and loaded back, renders
// the same samples as the untrimmed state (the anchor: a render of the untrimmed state through the same core but not
// through the files); a mini that lacks its ROM data renders differently; a track that needs no run doesn't run; a
// loop watch gives the length analysis its loops, from one jump back a pass or from one for each track of a pass; and
// StopThread unlinks libultra threads.

#include "rip/rip.h"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "xsflib/bytes.h"
#include "xsflib/files.h"
#include "xsflib/testing/check.h"
#include "xsflib/testing/temp_folder.h"

#include "n64/system.h"
#include "rip/mips.h"
#include "rip/snapshot.h"
#include "rip/trim.h"
#include "usf/usf_file.h"

namespace
{

namespace mips = usf::rip::mips;

// The program's memory, by physical address.
constexpr uint32_t kCode = 0x00001000;
constexpr uint32_t kSequence = 0x00100000; // Where the track's ROM bytes go.
constexpr uint32_t kGains = 0x00180000;    // One word for each track.
constexpr uint32_t kShared = 0x00181000;   // A word that every track reads.
constexpr uint32_t kBuffer = 0x00200000;

// The ROM's layout: each track's 256 bytes, and a word that every track reads.
constexpr uint32_t kTrackData = 0x1000;
constexpr uint32_t kSharedRom = 0x4000;

constexpr uint32_t kKseg0 = 0x80000000;
constexpr int kTracks = 3;

// The program, the address of its jump back to the start of its loop, and the address of the tracks' jumps back.
struct Program
{
    std::vector<uint32_t> words;
    uint32_t loop_jump = 0;
    uint32_t track_jump = 0;
};

// With `loop_tracks`, the end of each pass also takes a jump back for each of that many tracks, one after another, as
// a sequence player whose tracks loop separately does. At `track_jump`, t3 holds the track and t9 its loop's target.
Program MakeProgram(int loop_tracks = 0)
{
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

    mips::Assembler code(kKseg0 | kCode);

    // Copy the track's 256 bytes from the ROM into RDRAM, and wait for the DMA.
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

    // The gain: the track's entry in the table, the shared RDRAM word and the shared ROM word.
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

    // The AI at 48,681,812 / 1,521 Hz, about 32 kHz.
    code.Emit(mips::Addiu(kS0, kZero, 0));
    code.Emit(mips::Lui(kS2, 0xA450));
    code.Emit(mips::Addiu(kT0, kZero, 1520));
    code.Emit(mips::Sw(kT0, kS2, 0x10));
    code.Emit(mips::Addiu(kT0, kZero, 15));
    code.Emit(mips::Sw(kT0, kS2, 0x14));
    code.Emit(mips::Addiu(kT0, kZero, 1));
    code.Emit(mips::Sw(kT0, kS2, 0x8));

    // Each buffer: 256 frames, frame i from byte (counter + i) mod 256 plus the gain.
    const uint32_t loop = code.Here();
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

    // Hand the buffer to the AI, and wait while the AI is full.
    code.Emit(mips::Lui(kT0, kBuffer >> 16));
    code.Emit(mips::Sw(kT0, kS2, 0x0));
    code.Emit(mips::Addiu(kT0, kZero, 1024));
    code.Emit(mips::Sw(kT0, kS2, 0x4));
    code.Emit(mips::Addiu(kS0, kS0, 7));
    const uint32_t wait_ai = code.Here();
    code.Emit(mips::Lw(kT0, kS2, 0xC));
    code.Emit(mips::Srl(kT0, kT0, 31));
    code.Bne(kT0, kZero, wait_ai);
    code.Emit(mips::kNop);

    Program program;
    if (loop_tracks > 0)
    {
        code.Emit(mips::Addiu(kT3, kZero, 0));
        const uint32_t next_track = code.Here();
        code.Emit(mips::Sll(kT9, kT3, 4));
        program.track_jump = code.Here();
        code.Emit(mips::Addiu(kT3, kT3, 1));
        code.Emit(mips::Addiu(kT8, kZero, static_cast<uint16_t>(loop_tracks)));
        code.Bne(kT3, kT8, next_track);
        code.Emit(mips::kNop);
    }

    program.loop_jump = code.Here();
    code.J(loop);
    code.Emit(mips::kNop);
    program.words = code.Words();

    return program;
}

std::vector<uint8_t> MakeRom()
{
    std::vector<uint8_t> rom(0x8000, 0);
    xsflib::StoreBe32(rom.data(), 0x80371240);

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
    usf::n64::Options options;
    options.timing = usf::n64::Timing::kLegacy;
    usf::n64::System system(options);
    system.LoadRom(MakeRom());

    for (size_t i = 0; i < program.words.size(); i++)
    {
        system.WritePhysical32(kCode + 4 * static_cast<uint32_t>(i), program.words[i]);
    }

    for (uint32_t track = 0; track < kTracks; track++)
    {
        system.WritePhysical32(kGains + 4 * track, 3 + 2 * track);
    }
    system.WritePhysical32(kShared, 5);

    system.GetCpu().SetCop0(usf::n64::kCop0Status, 0);
    system.GetCpu().SetPc(0xFFFFFFFF00000000ull | kKseg0 | kCode);

    usf::rip::Snapshot snapshot;
    snapshot.rom = system.Rom();
    snapshot.state = system.SaveState();
    snapshot.selector_kind = usf::rip::TrackSelector::Kind::kRegister;
    snapshot.selector_offset = 0x050 + 8 * 4;
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

void TrimmedSet()
{
    const Program program = MakeProgram();
    const usf::rip::Snapshot snapshot = MakeSnapshot(program);
    const usf::rip::GameProfile profile = MakeProfile();
    usf::rip::RunSettings settings;
    settings.lengths = false;
    settings.margin_seconds = 1.5;

    std::vector<usf::rip::TrackRun> runs;
    std::vector<usf::rip::TrackWords> words;
    for (const usf::rip::Track& track : profile.tracks)
    {
        runs.push_back(usf::rip::RunTrack(profile, snapshot, track, settings));
        words.push_back(runs.back().words);
    }

    // Coverage: each track read the bytes of its number, the shared words and the code, but no other track's bytes,
    // and none of the RDRAM that the DMA or the program wrote before reading it.
    for (uint32_t track = 0; track < kTracks; track++)
    {
        const usf::rip::TrackWords& read = words[track];
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
    XSFLIB_CHECK(runs[0].reference.size() == 2 && runs[0].reference != runs[1].reference);

    // The set: the library holds the whole state and the shared ROM word. Each mini holds its track's ROM bytes and its
    // number in a0, the selector at state offset 0x070.
    const usf::rip::SetSections set = usf::rip::BuildSet(snapshot, profile.tracks, &words);
    XSFLIB_CHECK(set.library.rom && set.library.rom->size() == 1 && set.library.rom->front().offset == kSharedRom);
    XSFLIB_CHECK(set.library.state && set.library.state->size() == 1);
    XSFLIB_CHECK(set.library.state && set.library.state->front().offset == 0);
    XSFLIB_CHECK(set.library.state && set.library.state->front().data == snapshot.state);
    XSFLIB_CHECK(set.minis.size() == kTracks && set.minis[1].rom && set.minis[1].rom->size() == 1);
    XSFLIB_CHECK(set.minis[1].rom && set.minis[1].rom->front().offset == kTrackData + 0x100);
    for (uint32_t track = 0; track < kTracks; track++)
    {
        const usf::Sections& mini = set.minis[track];
        XSFLIB_CHECK(mini.state && mini.state->size() == 1);
        if (mini.state && mini.state->size() == 1)
        {
            const usf::Chunk& selector = mini.state->front();
            XSFLIB_CHECK(selector.offset == 0x070 && selector.data.size() == 8);
            XSFLIB_CHECK(selector.data.size() == 8 && xsflib::LoadLe64(selector.data.data()) == track);
        }
    }

    xsflib::testing::TempFolder folder("usf_rip_test");
    usf::rip::SetTags tags;
    tags.ripper = "usfrip test";
    const std::vector<uint8_t> library = usf::rip::LibraryFile(profile, set.library, tags);
    XSFLIB_CHECK(!xsflib::WriteFile(folder.Path() / "Test.usflib", library));

    for (size_t i = 0; i < profile.tracks.size(); i++)
    {
        const std::vector<uint8_t> mini =
            usf::rip::MiniFile(profile, set.minis[i], profile.tracks[i], i, runs[i].timing, "Test.usflib", tags);
        const std::filesystem::path path = folder.Path() / ("track" + std::to_string(i) + ".miniusf");
        XSFLIB_CHECK(!xsflib::WriteFile(path, mini));

        // The anchor: the untrimmed state's render.
        const auto problem = usf::rip::CheckTrack(xsflib::Utf8(path), runs[i]);
        XSFLIB_CHECK(!problem);
    }

    // The mini's tags, and a library that keeps a small part of the ROM.
    usf::LoadedUsf loaded;
    XSFLIB_CHECK(!usf::LoadUsf(xsflib::Utf8(folder.Path() / "track1.miniusf"), xsflib::psf::ReadWholeFile, loaded));
    XSFLIB_CHECK(loaded.enable_compare && loaded.enable_fifo_full);
    XSFLIB_CHECK(loaded.tags["title"] == "Track 1" && loaded.tags["length"] == "0:02.000");
    XSFLIB_CHECK(loaded.tags["usfrip"] == "usfrip test" && loaded.tags["_lib"] == "Test.usflib");
    XSFLIB_CHECK(loaded.rom.size() == kSharedRom + 4);

    // A mini without its ROM data plays the wrong bytes.
    usf::Sections broken = set.minis[2];
    broken.rom.reset();
    const std::filesystem::path broken_path = folder.Path() / "broken.miniusf";
    XSFLIB_CHECK(!xsflib::WriteFile(
        broken_path, usf::rip::MiniFile(profile, broken, profile.tracks[2], 2, runs[2].timing, "Test.usflib", tags)));
    XSFLIB_CHECK(usf::rip::CheckTrack(xsflib::Utf8(broken_path), runs[2]).has_value());
}

// A track with no length analysis, no coverage and no reference to make doesn't run. Its state here would make the
// core throw: its identifier is zero.
void NoRun()
{
    usf::rip::Snapshot snapshot;
    snapshot.state.assign(usf::kStateOverhead + 0x400000, 0);
    snapshot.selector_offset = 0x050 + 8 * 4;
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

// A loop watch at the jump back to the start of the program's loop: each buffer is a pass of the loop. The analysis
// counts two passes after the first sound and fades from there: a length of two or three buffers of 256 frames at
// 32,006 Hz, 8 ms each, and the default fade of 10 s.
void LoopWatch()
{
    const Program program = MakeProgram();
    const usf::rip::Snapshot snapshot = MakeSnapshot(program);
    usf::rip::GameProfile profile = MakeProfile();
    profile.tracks[0].length_seconds.reset();
    profile.loop_watches.push_back({kKseg0 | program.loop_jump, -1, -1});
    usf::rip::RunSettings settings;
    settings.coverage = false;
    settings.margin_seconds = 0.5;

    const usf::rip::TrackRun run = usf::rip::RunTrack(profile, snapshot, profile.tracks[0], settings);

    XSFLIB_CHECK(run.timing.has_value());
    if (run.timing)
    {
        XSFLIB_CHECK(run.timing->length_ms >= 15 && run.timing->length_ms <= 24);
        XSFLIB_CHECK(run.timing->fade_ms == 10000 && !run.timing->silent);
    }
}

// A loop watch at the tracks' jumps back, with the track in t3 and the target in t9: each buffer is a pass in which
// three tracks loop. The analysis counts the passes of track 0 and fades once each other track has looped twice too:
// two or three buffers, as with one jump back a pass.
void TrackLoops()
{
    const Program program = MakeProgram(3);
    const usf::rip::Snapshot snapshot = MakeSnapshot(program);
    usf::rip::GameProfile profile = MakeProfile();
    profile.tracks[0].length_seconds.reset();
    profile.loop_watches.push_back({kKseg0 | program.track_jump, mips::kT9, mips::kT3});
    usf::rip::RunSettings settings;
    settings.coverage = false;
    settings.margin_seconds = 0.5;

    const usf::rip::TrackRun run = usf::rip::RunTrack(profile, snapshot, profile.tracks[0], settings);

    XSFLIB_CHECK(run.timing.has_value());
    if (run.timing)
    {
        XSFLIB_CHECK(run.timing->length_ms >= 15 && run.timing->length_ms <= 24);
        XSFLIB_CHECK(run.timing->fade_ms == 10000 && !run.timing->silent);
    }
}

// libultra's OSThread: next at 0x00, priority at 0x04, queue at 0x08, state (16 bits) at 0x10. A queue ends at a
// thread of priority -1.
void StopThreads()
{
    usf::n64::System system;
    system.GetCpu().SetCop0(usf::n64::kCop0Status, 0);

    constexpr uint32_t kQueue = 0x00190000;
    constexpr uint32_t kThreads[] = {0x00190100, 0x00190200, 0x00190300};
    constexpr uint32_t kTail = 0x00190400;
    system.WritePhysical32(kQueue, kKseg0 | kThreads[0]);
    for (int i = 0; i < 3; i++)
    {
        system.WritePhysical32(kThreads[i], kKseg0 | (i < 2 ? kThreads[i + 1] : kTail));
        system.WritePhysical32(kThreads[i] + 4, 10);
        system.WritePhysical32(kThreads[i] + 8, kKseg0 | kQueue);
        system.WritePhysical32(kThreads[i] + 0x10, 0x00080000); // Waiting.
    }
    system.WritePhysical32(kTail + 4, 0xFFFFFFFF);

    usf::rip::StopThread(system, kKseg0 | kThreads[1]);
    usf::rip::StopThread(system, kKseg0 | kThreads[0]);

    XSFLIB_CHECK(system.ReadPhysical32(kQueue) == (kKseg0 | kThreads[2]));
    XSFLIB_CHECK(system.ReadPhysical32(kThreads[0] + 0x10) >> 16 == 1);
    XSFLIB_CHECK(system.ReadPhysical32(kThreads[1] + 0x10) >> 16 == 1);
    XSFLIB_CHECK(system.ReadPhysical32(kThreads[1] + 8) == 0);
    XSFLIB_CHECK(system.ReadPhysical32(kThreads[2]) == (kKseg0 | kTail));

    system.WritePhysical32(kThreads[2] + 0x10, 0x00040000); // Running.

    bool threw = false;
    try
    {
        usf::rip::StopThread(system, kKseg0 | kThreads[2]);
    }
    catch (const std::runtime_error&)
    {
        threw = true;
    }

    XSFLIB_CHECK(threw);
}

// The selector in RDRAM: a mini writes the number into its bytes, in LE word order.
void RdramSelector()
{
    usf::rip::Snapshot snapshot;
    snapshot.state.assign(usf::kStateOverhead + 0x400000, 0);
    snapshot.selector_kind = usf::rip::TrackSelector::Kind::kRdram;
    snapshot.selector_address = 0x1002;
    snapshot.selector_value_size = 2;
    snapshot.selector_offset = 0x75C + 0x1000;
    snapshot.selector_size = 4;

    const std::vector<uint8_t> state = usf::rip::TrackState(snapshot, 0xABCD);

    // Console bytes 0x1002 and 0x1003 hold 0xAB and 0xCD, at state positions 0x75C + (A XOR 3).
    XSFLIB_CHECK(state[0x75C + (0x1002 ^ 3)] == 0xAB && state[0x75C + (0x1003 ^ 3)] == 0xCD);
    XSFLIB_CHECK(xsflib::LoadLe32(state.data() + 0x75C + 0x1000) == 0x0000ABCD);
}

// A track's patches: the mini holds them beside the selector, and they replace what the snapshot holds.
void TrackPatches()
{
    usf::rip::Snapshot snapshot;
    snapshot.state.assign(usf::kStateOverhead + 0x400000, 0);
    xsflib::StoreLe32(snapshot.state.data() + 0x75C + 0x2004, 0x11111111);
    snapshot.selector_kind = usf::rip::TrackSelector::Kind::kRdram;
    snapshot.selector_address = 0x1000;
    snapshot.selector_value_size = 4;
    snapshot.selector_offset = 0x75C + 0x1000;
    snapshot.selector_size = 4;

    usf::rip::Track track;
    track.number = 7;
    track.rdram_patches = {{.address = 0x80002000, .words = {0x01020304, 0x05060708}},
                           {.address = 0xA0003000, .words = {0x090A0B0C}}};

    const std::vector<uint8_t> state = usf::rip::TrackState(snapshot, track);

    XSFLIB_CHECK(xsflib::LoadLe32(state.data() + 0x75C + 0x1000) == 7);
    XSFLIB_CHECK(xsflib::LoadLe32(state.data() + 0x75C + 0x2000) == 0x01020304);
    XSFLIB_CHECK(xsflib::LoadLe32(state.data() + 0x75C + 0x2004) == 0x05060708);
    XSFLIB_CHECK(xsflib::LoadLe32(state.data() + 0x75C + 0x3000) == 0x090A0B0C);

    // The console's bytes, in LE word order.
    XSFLIB_CHECK(state[0x75C + (0x2000 ^ 3)] == 0x01 && state[0x75C + (0x2003 ^ 3)] == 0x04);

    const usf::rip::SetSections set = usf::rip::BuildSet(snapshot, {track}, nullptr);

    XSFLIB_CHECK(set.minis.size() == 1 && set.minis[0].state && set.minis[0].state->size() == 3);
    XSFLIB_CHECK(set.minis[0].state && set.minis[0].state->at(0).offset == 0x75C + 0x1000);
    XSFLIB_CHECK(set.minis[0].state && set.minis[0].state->at(1).offset == 0x75C + 0x2000);
    XSFLIB_CHECK(set.minis[0].state && set.minis[0].state->at(1).data.size() == 8);
    XSFLIB_CHECK(set.minis[0].state && set.minis[0].state->at(2).offset == 0x75C + 0x3000);

    bool threw = false;
    try
    {
        usf::rip::Track bad;
        bad.rdram_patches = {{.address = 0x00002000, .words = {1}}};
        (void)usf::rip::TrackState(snapshot, bad);
    }
    catch (const std::runtime_error&)
    {
        threw = true;
    }

    XSFLIB_CHECK(threw);
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("rip",
                                    []
                                    {
                                        TrimmedSet();
                                        NoRun();
                                        LoopWatch();
                                        TrackLoops();
                                        StopThreads();
                                        RdramSelector();
                                        TrackPatches();
                                    });
}
