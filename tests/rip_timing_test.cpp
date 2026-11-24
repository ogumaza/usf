// SPDX-License-Identifier: MIT

// The timings of a rip on a small program made with the MIPS encoder. The program plays silence, then a constant level
// from one frame of its output to another, on both channels or on the right one alone, then silence again. A track may
// change the AI's rate at the start of its sound.
//
// The anchor: each track's frames rendered one at a time through the ripper's player, each counted at the rate that it
// played at, without the millisecond renderer of the analysis. The checks:
// - the first sample's time, on either channel and across a change of rate;
// - the length of a track that ends at its last sound within the profile's length;
// - a profile's loop that keeps its length;
// - a track whose first sample comes after its length;
// - a track with no sound: it is silent and gets no mini;
// - the tags, counted from the first sample;
// - the reference of an analysed track: it runs to the end of the fade, counted from the start of the track.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "ultralib/machine/system.h"
#include "ultralib/testing/test_programs.h"
#include "xsflib/cli/rip.h"
#include "xsflib/psf/psf.h"
#include "xsflib/rip/file_names.h"
#include "xsflib/rip/length.h"
#include "xsflib/testing/check.h"
#include "xsflib/testing/temp_folder.h"

#include "rip/mips.h"
#include "rip/player.h"
#include "rip/profile.h"
#include "rip/rip.h"
#include "rip/snapshot.h"
#include "test_mips.h"

namespace
{

namespace mips = usf::rip::mips;

using mips::kA0;
using mips::kS0;
using mips::kS2;
using mips::kS3;
using mips::kS4;
using mips::kS5;
using mips::kS6;
using mips::kS7;
using mips::kT0;
using mips::kT1;
using mips::kT2;
using mips::kT3;
using mips::kT4;
using mips::kT5;
using mips::kZero;
using ultralib::machine::kKseg0;
using ultralib::testing::kCode;

// The program's memory, by physical address: a table entry for each track, and the buffer.
constexpr uint32_t kTable = 0x00180000;
constexpr uint32_t kBuffer = 0x00200000;

// A table entry: the first frame of the sound, the frame after its last, the sample word of the sound, the AI's
// DACRATE from the buffer kRateBuffer on (0 for none), and that buffer.
constexpr uint32_t kEntryShift = 5;
constexpr uint16_t kStartFrame = 0x0;
constexpr uint16_t kEndFrame = 0x4;
constexpr uint16_t kWord = 0x8;
constexpr uint16_t kRate = 0xC;
constexpr uint16_t kRateBuffer = 0x10;

constexpr uint32_t kFramesPerBuffer = 256;

// About 32 kHz, then about 16 kHz.
constexpr uint16_t kDacRate = 1520;
constexpr uint16_t kSlowDacRate = 3041;

// The sample words: 4096 on both channels, or on the right one alone. The AI takes the left sample from the high half.
constexpr uint32_t kBoth = 0x10001000;
constexpr uint32_t kRightOnly = 0x00001000;

// A track: its name, its table entry, and the length that the profile gives it.
struct TrackSound
{
    const char* name;
    uint32_t start_frame;
    uint32_t end_frame;
    uint32_t word;
    uint32_t rate;
    uint32_t rate_buffer;
    double length_seconds;
};

// The tracks, by number.
enum TrackNumber : uint32_t
{
    kSound,
    kRight,
    kNoSound,
    kLate,
    kRateChange,
};

// The first sample of kSound is 31.56 ms after the start: the millisecond of the analysis starts more than 0.5 ms
// before it. kLate sounds only after its length. kRateChange plays three buffers at the first rate, then changes the
// rate, and starts its sound a buffer later.
constexpr std::array<TrackSound, 5> kTrackSounds = {{
    {"Sound", 1010, 10000, kBoth, 0, 0, 2.0},
    {"Right", 1200, 10000, kRightOnly, 0, 0, 2.0},
    {"No sound", 0, 0, kBoth, 0, 0, 2.0},
    {"Late", 10000, 12000, kBoth, 0, 0, 0.1},
    {"Rate", 4 * kFramesPerBuffer, 10000, kBoth, kSlowDacRate, 3, 2.0},
}};

// The program, and the address of its jump back to the start of its loop: one pass for each buffer.
struct Program
{
    std::vector<uint32_t> words;
    uint32_t loop_jump = 0;
};

// At the start of the buffer kRateBuffer, the track's DACRATE, once the AI has played what it contains.
void EmitRateChange(mips::Assembler& code)
{
    code.Emit(mips::Lw(kT0, kS3, kRateBuffer));
    const size_t same_rate = code.Size();
    code.Bne(kS0, kT0, code.Here() + 4);
    code.Emit(mips::kNop);
    const size_t no_rate = code.Size();
    code.Beq(kS7, kZero, code.Here() + 4);
    code.Emit(mips::kNop);
    usf::testing::EmitAiWait(code, 30);
    code.Emit(mips::Sw(kS7, kS2, 0x10));
    code.SetBranchTarget(same_rate, code.Here());
    code.SetBranchTarget(no_rate, code.Here());
}

// The buffer: frame by frame, the track's sample word from its first frame to its last, and 0 elsewhere. s5 counts the
// frames of the track.
void EmitFill(mips::Assembler& code)
{
    code.Emit(mips::Lui(kT0, mips::High(kKseg0 | kBuffer)));
    code.Emit(mips::Addiu(kT1, kZero, 0));
    code.Emit(mips::Addiu(kT5, kZero, kFramesPerBuffer * 4));
    const uint32_t fill = code.Here();
    code.Emit(mips::Addiu(kT2, kZero, 0));
    code.Emit(mips::Sltu(kT3, kS5, kS4));
    const size_t before = code.Size();
    code.Bne(kT3, kZero, code.Here() + 4);
    code.Emit(mips::kNop);
    code.Emit(mips::Sltu(kT3, kS5, kS6));
    const size_t after = code.Size();
    code.Beq(kT3, kZero, code.Here() + 4);
    code.Emit(mips::kNop);
    code.Emit(mips::Lw(kT2, kS3, kWord));
    code.SetBranchTarget(before, code.Here());
    code.SetBranchTarget(after, code.Here());
    code.Emit(mips::Addu(kT4, kT0, kT1));
    code.Emit(mips::Sw(kT2, kT4, 0));
    code.Emit(mips::Addiu(kS5, kS5, 1));
    code.Emit(mips::Addiu(kT1, kT1, 4));
    code.Bne(kT1, kT5, fill);
    code.Emit(mips::kNop);
}

Program MakeProgram()
{
    mips::Assembler code(kKseg0 | kCode);
    usf::testing::EmitAiSetup(code, mips::Addiu(kT0, kZero, kDacRate));

    // s3: the track's entry. s4 and s6: its first frame and the frame after its last. s7: its DACRATE.
    code.LoadImmediate(kS3, kKseg0 | kTable);
    code.Emit(mips::Sll(kT0, kA0, kEntryShift));
    code.Emit(mips::Addu(kS3, kS3, kT0));
    code.Emit(mips::Lw(kS4, kS3, kStartFrame));
    code.Emit(mips::Lw(kS6, kS3, kEndFrame));
    code.Emit(mips::Lw(kS7, kS3, kRate));
    code.Emit(mips::Addiu(kS0, kZero, 0));
    code.Emit(mips::Addiu(kS5, kZero, 0));

    const uint32_t loop = code.Here();
    EmitRateChange(code);
    EmitFill(code);
    usf::testing::EmitAiBuffer(code, kBuffer >> 16, kFramesPerBuffer * 4);
    code.Emit(mips::Addiu(kS0, kS0, 1));
    usf::testing::EmitAiWait(code, 31);

    Program program;
    program.loop_jump = code.Here();
    code.J(loop);
    code.Emit(mips::kNop);
    program.words = code.Words();

    return program;
}

// The snapshot: the program about to start, with the track's number in a0 and the table in RDRAM.
usf::rip::Snapshot MakeSnapshot(const Program& program)
{
    ultralib::machine::System system(ultralib::testing::LegacyOptions());
    system.LoadRom(ultralib::testing::TestRom(0x8000));
    ultralib::testing::StartProgram(system, program.words, 0);
    for (uint32_t track = 0; track < kTrackSounds.size(); track++)
    {
        const TrackSound& sound = kTrackSounds[track];
        const uint32_t entry = kTable + (track << kEntryShift);
        system.WritePhysical32(entry + kStartFrame, sound.start_frame);
        system.WritePhysical32(entry + kEndFrame, sound.end_frame);
        system.WritePhysical32(entry + kWord, sound.word);
        system.WritePhysical32(entry + kRate, sound.rate);
        system.WritePhysical32(entry + kRateBuffer, sound.rate_buffer);
    }

    return usf::testing::A0Snapshot(system);
}

usf::rip::GameProfile MakeProfile()
{
    usf::rip::GameProfile profile;
    profile.game = "Test";
    for (uint32_t track = 0; track < kTrackSounds.size(); track++)
    {
        usf::rip::Track entry;
        entry.number = track;
        entry.name = kTrackSounds[track].name;
        entry.length_seconds = kTrackSounds[track].length_seconds;
        profile.tracks.push_back(entry);
    }

    return profile;
}

// The anchor of a track's sounds over its first `ms` milliseconds.
struct Sounds
{
    std::optional<double> first_sample_ms; // The start of the first frame with a sample that isn't 0.
    std::optional<double> sound_end_ms;    // The end of the last frame with a sample over -72 dB.
    bool left = false;                     // Whether a left sample isn't 0.
    bool right = false;
    uint32_t rate = 0; // The rate of the last frame.
};

Sounds SoundsOf(const usf::rip::Snapshot& snapshot, const usf::rip::Track& track, double ms)
{
    usf::rip::TrackPlayer player(snapshot.rom, usf::rip::TrackState(snapshot, track), {});
    Sounds sounds;
    std::array<int16_t, 2> frame{};
    double time_ms = 0.0;
    while (time_ms < ms)
    {
        player.Render(frame);
        sounds.rate = player.SampleRate();
        const double end_ms = time_ms + 1000.0 / sounds.rate;
        if ((frame[0] != 0 || frame[1] != 0) && !sounds.first_sample_ms)
        {
            sounds.first_sample_ms = time_ms;
        }
        if (xsflib::rip::Audible(frame))
        {
            sounds.sound_end_ms = end_ms;
        }

        sounds.left = sounds.left || frame[0] != 0;
        sounds.right = sounds.right || frame[1] != 0;
        time_ms = end_ms;
    }

    return sounds;
}

// A time in milliseconds, rounded as the analysis rounds it.
long long Rounded(double ms)
{
    return static_cast<long long>(ms + 0.5);
}

// Runs a track as usfrip runs one whose length the profile sets, with no coverage and no reference.
usf::rip::TrackRun RunSet(const usf::rip::Snapshot& snapshot, const usf::rip::GameProfile& profile,
                          const usf::rip::Track& track)
{
    usf::rip::RunSettings settings;
    settings.lengths = false;
    settings.coverage = false;
    settings.reference = false;

    return usf::rip::RunTrack(profile, snapshot, track, settings);
}

// A track that ends within the profile's length: its first sample, its first sound over -72 dB, and a length at its
// last sound. The first sample on the right channel alone counts too.
void Endings(const usf::rip::Snapshot& snapshot, const usf::rip::GameProfile& profile)
{
    for (const TrackNumber number : {kSound, kRight})
    {
        const usf::rip::Track& track = profile.tracks[number];
        const Sounds sounds = SoundsOf(snapshot, track, 1000.0);
        XSFLIB_REQUIRE(sounds.first_sample_ms && sounds.sound_end_ms);
        XSFLIB_CHECK(sounds.right && sounds.left == (number == kSound));

        const usf::rip::TrackRun run = RunSet(snapshot, profile, track);

        XSFLIB_REQUIRE(run.timing && run.timing->first_sample_ms && run.timing->first_sound_ms);
        const xsflib::rip::Timing& timing = *run.timing;
        XSFLIB_CHECK(!timing.silent && timing.fade_ms == 0);
        XSFLIB_CHECK(*timing.first_sample_ms == Rounded(*sounds.first_sample_ms));
        XSFLIB_CHECK(*timing.first_sound_ms >= *timing.first_sample_ms - 1 &&
                     *timing.first_sound_ms <= *timing.first_sample_ms);
        XSFLIB_CHECK(static_cast<double>(timing.length_ms) >= *sounds.sound_end_ms &&
                     static_cast<double>(timing.length_ms) < *sounds.sound_end_ms + 1.0);
    }
}

// A track that the profile gives a length and a fade loops: it keeps its length past its last sound.
void SetLoop(const usf::rip::Snapshot& snapshot, const usf::rip::GameProfile& profile)
{
    usf::rip::Track track = profile.tracks[kSound];
    track.fade_seconds = 7.0;

    const usf::rip::TrackRun run = RunSet(snapshot, profile, track);

    XSFLIB_REQUIRE(run.timing && run.timing->first_sample_ms);
    XSFLIB_CHECK(run.timing->length_ms == 2000 && run.timing->fade_ms == 7000 && !run.timing->silent);
}

// A track whose first sample comes after the profile's length runs on to it, and isn't silent.
void LateTrack(const usf::rip::Snapshot& snapshot, const usf::rip::GameProfile& profile)
{
    const usf::rip::Track& track = profile.tracks[kLate];
    const Sounds sounds = SoundsOf(snapshot, track, 500.0);
    XSFLIB_REQUIRE(sounds.first_sample_ms && *sounds.first_sample_ms > 300.0);

    const usf::rip::TrackRun run = RunSet(snapshot, profile, track);

    XSFLIB_REQUIRE(run.timing && run.timing->first_sample_ms);
    XSFLIB_CHECK(!run.timing->silent && *run.timing->first_sample_ms == Rounded(*sounds.first_sample_ms));
}

// The first sample after a change of rate: the frames before the change count at the first rate, to within the
// millisecond of the change. Counted at the second rate, the first sample would be 24 ms late.
void RateChange(const usf::rip::Snapshot& snapshot, const usf::rip::GameProfile& profile)
{
    const usf::rip::Track& track = profile.tracks[kRateChange];
    const Sounds sounds = SoundsOf(snapshot, track, 200.0);
    const Sounds first_rate = SoundsOf(snapshot, profile.tracks[kSound], 10.0);
    XSFLIB_REQUIRE(sounds.first_sample_ms);
    XSFLIB_CHECK(sounds.rate < first_rate.rate * 3 / 4);

    const usf::rip::TrackRun run = RunSet(snapshot, profile, track);

    XSFLIB_REQUIRE(run.timing && run.timing->first_sample_ms);
    XSFLIB_CHECK(*run.timing->first_sample_ms >= Rounded(*sounds.first_sample_ms) - 1 &&
                 *run.timing->first_sample_ms <= Rounded(*sounds.first_sample_ms) + 1);
}

// A mini to write: the number in its file's name, its title, its timing and its position among the minis. usfrip
// passes the same fields to SoundingJobs and xsflib's WriteMinis.
struct MiniJob
{
    uint32_t index = 0;
    std::string label;
    std::optional<xsflib::rip::Timing> timing;
    size_t position = 0;
};

// A track with no sound is silent, and gets no mini. The other minis' tags count from their first samples.
void MiniTags(const usf::rip::Snapshot& snapshot, const usf::rip::GameProfile& profile)
{
    std::vector<MiniJob> jobs;
    for (const TrackNumber number : {kSound, kRight, kNoSound, kRateChange})
    {
        const usf::rip::Track& track = profile.tracks[number];
        jobs.push_back({number + 1, track.name, RunSet(snapshot, profile, track).timing, jobs.size()});
    }

    XSFLIB_REQUIRE(jobs[2].timing);
    XSFLIB_CHECK(jobs[2].timing->silent && !jobs[2].timing->first_sample_ms);

    const std::vector<MiniJob> sounding = usf::rip::SoundingJobs(jobs);

    XSFLIB_REQUIRE(sounding.size() == 3);
    XSFLIB_CHECK(sounding[0].label == "Sound" && sounding[1].label == "Right" && sounding[2].label == "Rate");
    XSFLIB_CHECK(sounding[0].position == 0 && sounding[1].position == 1 && sounding[2].position == 2);

    // The minis, written as usfrip writes them. The test keeps each mini's tags.
    std::map<std::string, xsflib::psf::Tags> tags;
    const auto build = [&](const MiniJob& job, const xsflib::psf::Tags& mini_tags)
    {
        tags[job.label] = mini_tags;
        return std::vector<uint8_t>{0};
    };
    xsflib::testing::TempFolder folder("usf_rip_timing_test");
    xsflib::rip::FileNames names(".usflib", ".miniusf");
    xsflib::cli::MiniSettings settings;
    settings.stream = stdout;

    const int written =
        xsflib::cli::WriteMinis(folder.Path(), names, sounding, {}, usf::rip::kLengthFrom, settings, build);

    XSFLIB_REQUIRE(written == 3 && tags.size() == 3);
    for (const MiniJob& job : sounding)
    {
        const xsflib::rip::Timing& timing = *job.timing;
        XSFLIB_CHECK(tags[job.label]["length"] == xsflib::psf::FormatTime(timing.length_ms - *timing.first_sample_ms));
        XSFLIB_CHECK(tags[job.label]["fade"] == "1" && tags[job.label].count("loop_start") == 0);
    }
}

// An analysed track with a loop watch at the program's jump back: a loop that starts with the sound. Its first sample
// comes from the analysis, and its reference covers the frames up to the end of its fade, counted from the start of the
// track.
void AnalysedTrack(const Program& program, const usf::rip::Snapshot& snapshot)
{
    usf::rip::GameProfile profile = MakeProfile();
    profile.loop_watches.push_back({kKseg0 | program.loop_jump, -1, -1});
    usf::rip::Track track = profile.tracks[kSound];
    track.length_seconds.reset();
    const Sounds sounds = SoundsOf(snapshot, track, 100.0);
    XSFLIB_REQUIRE(sounds.first_sample_ms);
    usf::rip::RunSettings settings;
    settings.coverage = false;
    settings.margin_seconds = 0.5;

    const usf::rip::TrackRun run = usf::rip::RunTrack(profile, snapshot, track, settings);

    XSFLIB_REQUIRE(run.timing && run.timing->first_sample_ms);
    const xsflib::rip::Timing& timing = *run.timing;
    XSFLIB_CHECK(timing.fade_ms == 7000 && !timing.silent);
    XSFLIB_CHECK(*timing.first_sample_ms == Rounded(*sounds.first_sample_ms));
    const double end_frames = static_cast<double>(timing.length_ms + timing.fade_ms) * sounds.rate / 1000.0;
    XSFLIB_CHECK(static_cast<double>(run.check_frames) >= end_frames - sounds.rate / 1000.0 &&
                 static_cast<double>(run.check_frames) <= end_frames + sounds.rate / 1000.0);
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("rip timing",
                                    []
                                    {
                                        const Program program = MakeProgram();
                                        const usf::rip::Snapshot snapshot = MakeSnapshot(program);
                                        const usf::rip::GameProfile profile = MakeProfile();
                                        Endings(snapshot, profile);
                                        SetLoop(snapshot, profile);
                                        LateTrack(snapshot, profile);
                                        RateChange(snapshot, profile);
                                        MiniTags(snapshot, profile);
                                        AnalysedTrack(program, snapshot);
                                    });
}
