// SPDX-License-Identifier: MIT

// The player on small programs made with the MIPS encoder. The anchor of the checks is the arithmetic of the programs
// and the AI's registers: the samples that a program writes, and a rate of the video clock over DACRATE + 1.
//
// The main program takes a start value in a0 and plays buffers of 256 frames. Frame n of the stream holds start + n
// on the left and start + n + 1 on the right. Each buffer starts when the one before has ended. The program changes
// the rate between two buffers, and later stops sending buffers. The second program measures how long the AI stays
// busy, for a check of the flags.

#include "usf/player.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "xsflib/bytes.h"
#include "xsflib/files.h"
#include "xsflib/player/player.h"
#include "xsflib/psf/psf.h"
#include "xsflib/testing/check.h"
#include "xsflib/testing/temp_folder.h"

#include "n64/system.h"
#include "rip/mips.h"
#include "usf/usf_file.h"

namespace
{

namespace mips = usf::rip::mips;

constexpr uint32_t kCode = 0x00001000;
constexpr uint32_t kBuffer = 0x00200000;
constexpr uint32_t kKseg0 = 0x80000000;

constexpr uint32_t kFramesPerBuffer = 256;
constexpr uint32_t kBuffersBeforeChange = 4; // The rate changes after this many buffers.
constexpr uint32_t kBuffersBeforeStop = 8;   // The program stops sending buffers after this many.
constexpr uint32_t kFramesBeforeChange = kBuffersBeforeChange * kFramesPerBuffer;
constexpr uint32_t kFramesBeforeStop = kBuffersBeforeStop * kFramesPerBuffer;

constexpr uint32_t kDacRateFirst = 1520;
constexpr uint32_t kDacRateSecond = 2204;
constexpr uint32_t kVideoClock = 48681812;
constexpr uint32_t kRateFirst = kVideoClock / (kDacRateFirst + 1);
constexpr uint32_t kRateSecond = kVideoClock / (kDacRateSecond + 1);

constexpr uint32_t kStateA0 = 0x050 + 8 * 4; // The state image's position of register a0.
constexpr uint16_t kStart = 0x1200;

// Starts the AI: s2 ends up with the base of its registers.
void EmitAiSetup(mips::Assembler& code)
{
    using mips::kS2;
    using mips::kT0;
    using mips::kZero;

    code.Emit(mips::Lui(kS2, 0xA450));
    code.Emit(mips::Addiu(kT0, kZero, kDacRateFirst));
    code.Emit(mips::Sw(kT0, kS2, 0x10));
    code.Emit(mips::Addiu(kT0, kZero, 15));
    code.Emit(mips::Sw(kT0, kS2, 0x14));
    code.Emit(mips::Addiu(kT0, kZero, 1));
    code.Emit(mips::Sw(kT0, kS2, 0x8));
}

std::vector<uint32_t> MakeCountingProgram()
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
    using mips::kZero;

    mips::Assembler code(kKseg0 | kCode);
    EmitAiSetup(code);
    code.Emit(mips::Addu(kS0, kA0, kZero));
    code.Emit(mips::Addiu(kS1, kZero, 0));

    // Each buffer: frame i holds the counter + i on the left and one more on the right.
    const uint32_t loop = code.Here();
    code.Emit(mips::Lui(kT0, mips::High(kKseg0 | kBuffer)));
    code.Emit(mips::Addiu(kT1, kZero, 0));
    const uint32_t fill = code.Here();
    code.Emit(mips::Addu(kT2, kS0, kT1));
    code.Emit(mips::Andi(kT2, kT2, 0xFFFF));
    code.Emit(mips::Addiu(kT3, kT2, 1));
    code.Emit(mips::Andi(kT3, kT3, 0xFFFF));
    code.Emit(mips::Sll(kT2, kT2, 16));
    code.Emit(mips::Or(kT2, kT2, kT3));
    code.Emit(mips::Sll(kT4, kT1, 2));
    code.Emit(mips::Addu(kT4, kT4, kT0));
    code.Emit(mips::Sw(kT2, kT4, 0));
    code.Emit(mips::Addiu(kT1, kT1, 1));
    code.Emit(mips::Addiu(kT5, kZero, kFramesPerBuffer));
    code.Bne(kT1, kT5, fill);
    code.Emit(mips::kNop);

    // Hand the buffer to the AI, and wait until it has ended.
    code.Emit(mips::Lui(kT0, kBuffer >> 16));
    code.Emit(mips::Sw(kT0, kS2, 0x0));
    code.Emit(mips::Addiu(kT0, kZero, kFramesPerBuffer * 4));
    code.Emit(mips::Sw(kT0, kS2, 0x4));
    code.Emit(mips::Addiu(kS0, kS0, kFramesPerBuffer));
    code.Emit(mips::Addiu(kS1, kS1, 1));
    const uint32_t wait = code.Here();
    code.Emit(mips::Lw(kT0, kS2, 0xC));
    code.Emit(mips::Srl(kT0, kT0, 30));
    code.Bne(kT0, kZero, wait);
    code.Emit(mips::kNop);

    // The rate changes between two buffers.
    code.Emit(mips::Addiu(kT0, kZero, kBuffersBeforeChange));
    const size_t same_rate = code.Size();
    code.Bne(kS1, kT0, code.Here() + 4);
    code.Emit(mips::kNop);
    code.Emit(mips::Addiu(kT0, kZero, kDacRateSecond));
    code.Emit(mips::Sw(kT0, kS2, 0x10));
    code.SetBranchTarget(same_rate, code.Here());

    // The program stops sending buffers, but it goes on running.
    code.Emit(mips::Addiu(kT0, kZero, kBuffersBeforeStop));
    code.Bne(kS1, kT0, loop);
    code.Emit(mips::kNop);
    const uint32_t spin = code.Here();
    code.J(spin);
    code.Emit(mips::kNop);

    return code.Words();
}

// One buffer, then a count in s0 of the times that the AI read as busy, then a buffer of samples that hold that count,
// capped at 0x7FFF. The AI then stays quiet.
std::vector<uint32_t> MakeBusyProgram()
{
    using mips::kS0;
    using mips::kS2;
    using mips::kT0;
    using mips::kT1;
    using mips::kT2;
    using mips::kT3;
    using mips::kZero;

    mips::Assembler code(kKseg0 | kCode);
    EmitAiSetup(code);

    code.Emit(mips::Lui(kT0, kBuffer >> 16));
    code.Emit(mips::Sw(kT0, kS2, 0x0));
    code.Emit(mips::Addiu(kT0, kZero, kFramesPerBuffer * 4));
    code.Emit(mips::Sw(kT0, kS2, 0x4));
    code.Emit(mips::Addiu(kS0, kZero, 0));
    const uint32_t wait = code.Here();
    code.Emit(mips::Addiu(kS0, kS0, 1));
    code.Emit(mips::Lw(kT0, kS2, 0xC));
    code.Emit(mips::Srl(kT0, kT0, 30));
    code.Bne(kT0, kZero, wait);
    code.Emit(mips::kNop);

    // The cap.
    code.Emit(mips::Addiu(kT1, kZero, 0x7FFF));
    code.Emit(mips::Sltu(kT2, kT1, kS0));
    const size_t below_cap = code.Size();
    code.Beq(kT2, kZero, code.Here() + 4);
    code.Emit(mips::kNop);
    code.Emit(mips::Addu(kS0, kT1, kZero));
    code.SetBranchTarget(below_cap, code.Here());

    // The buffer.
    code.Emit(mips::Sll(kT2, kS0, 16));
    code.Emit(mips::Or(kT2, kT2, kS0));
    code.Emit(mips::Lui(kT0, mips::High(kKseg0 | kBuffer)));
    code.Emit(mips::Addiu(kT1, kZero, 0));
    code.Emit(mips::Addiu(kT3, kZero, kFramesPerBuffer * 4));
    const uint32_t fill = code.Here();
    code.Emit(mips::Addu(kS0, kT0, kT1));
    code.Emit(mips::Sw(kT2, kS0, 0));
    code.Emit(mips::Addiu(kT1, kT1, 4));
    code.Bne(kT1, kT3, fill);
    code.Emit(mips::kNop);
    code.Emit(mips::Lui(kT0, kBuffer >> 16));
    code.Emit(mips::Sw(kT0, kS2, 0x0));
    code.Emit(mips::Sw(kT3, kS2, 0x4));
    const uint32_t spin = code.Here();
    code.J(spin);
    code.Emit(mips::kNop);

    return code.Words();
}

std::vector<uint8_t> MakeRom()
{
    std::vector<uint8_t> rom(0x1000, 0);
    xsflib::StoreBe32(rom.data(), 0x80371240);

    return rom;
}

// A state image with `words` about to run from kCode, with no interrupts.
std::vector<uint8_t> MakeState(const std::vector<uint32_t>& words)
{
    usf::n64::Options options;
    options.timing = usf::n64::Timing::kLegacy;
    usf::n64::System system(options);
    system.LoadRom(MakeRom());

    for (size_t i = 0; i < words.size(); i++)
    {
        system.WritePhysical32(kCode + 4 * static_cast<uint32_t>(i), words[i]);
    }

    system.GetCpu().SetCop0(usf::n64::kCop0Status, 0);
    system.GetCpu().SetPc(0xFFFFFFFF00000000ull | kKseg0 | kCode);

    return system.SaveState();
}

// A library that holds the ROM and the state, and a mini that holds kStart in a0, the tags and a link to the library.
class TestSet
{
public:
    TestSet(const std::vector<uint32_t>& words, xsflib::psf::Tags mini_tags) : folder_("usf_player_test")
    {
        usf::Sections library;
        library.rom = std::vector<usf::Chunk>{{0, MakeRom()}};
        library.state = std::vector<usf::Chunk>{{0, MakeState(words)}};
        XSFLIB_CHECK(!xsflib::WriteFile(folder_.Path() / "Test.usflib", usf::WriteUsf(library, {})));

        std::vector<uint8_t> a0(8, 0);
        xsflib::StoreLe64(a0.data(), kStart);
        usf::Sections mini;
        mini.state = std::vector<usf::Chunk>{{kStateA0, a0}};
        mini_tags["_lib"] = "Test.usflib";
        XSFLIB_CHECK(!xsflib::WriteFile(MiniPath(), usf::WriteUsf(mini, mini_tags)));
    }

    std::filesystem::path MiniPath() const
    {
        return folder_.Path() / "Test.miniusf";
    }

    const std::filesystem::path& Folder() const
    {
        return folder_.Path();
    }

    void Open(usf::Player& player) const
    {
        XSFLIB_CHECK(player.Load(MiniPath().string(), xsflib::psf::ReadWholeFile));
        XSFLIB_CHECK(player.Start());
    }

private:
    xsflib::testing::TempFolder folder_;
};

int16_t Left(uint64_t frame)
{
    return static_cast<int16_t>(static_cast<uint16_t>(kStart + frame));
}

int16_t Right(uint64_t frame)
{
    return static_cast<int16_t>(static_cast<uint16_t>(kStart + frame + 1));
}

// Renders `count` frames of the stream from `first` on, and checks them against the program's arithmetic.
void CheckFrames(usf::Player& player, uint64_t first, uint32_t count)
{
    std::vector<int16_t> frames(size_t{count} * 2);
    XSFLIB_CHECK(player.Render(frames.data(), count) == count);

    bool same = true;
    for (uint32_t i = 0; i < count; i++)
    {
        same = same && frames[size_t{i} * 2] == Left(first + i) && frames[size_t{i} * 2 + 1] == Right(first + i);
    }
    XSFLIB_CHECK(same);
}

void PlaysAChain()
{
    const TestSet set(MakeCountingProgram(), {{"length", "0:10"}, {"fade", "1"}, {"title", "A track"}});
    usf::Player player;

    set.Open(player);

    XSFLIB_CHECK(player.GetState() == xsflib::player::State::kPlaying);
    XSFLIB_CHECK(player.GetTags().at("title") == "A track");
    XSFLIB_CHECK(player.LengthMs() == 10000 && player.FadeMs() == 1000);
    XSFLIB_CHECK(player.SampleRate() == kRateFirst);
    XSFLIB_CHECK(player.Position() == 0);

    // The size of a call doesn't matter.
    CheckFrames(player, 0, 1);
    CheckFrames(player, 1, 100);
    CheckFrames(player, 101, 400);
    XSFLIB_CHECK(player.Position() == 501);
}

void RateChanges()
{
    const TestSet set(MakeCountingProgram(), {});
    usf::Player player;
    set.Open(player);
    std::vector<int16_t> frames(size_t{kFramesBeforeChange + 100} * 2);

    // A call across the change stops at the change.
    const size_t got = player.Render(frames.data(), kFramesBeforeChange + 100);

    XSFLIB_CHECK(got == kFramesBeforeChange);
    XSFLIB_CHECK(frames[0] == Left(0) && frames[size_t{kFramesBeforeChange - 1} * 2] == Left(kFramesBeforeChange - 1));
    XSFLIB_CHECK(player.SampleRate() == kRateSecond);
    CheckFrames(player, kFramesBeforeChange, 100);
    XSFLIB_CHECK(player.SampleRate() == kRateSecond);
}

void StoppedGameGivesSilence()
{
    const TestSet set(MakeCountingProgram(), {});
    usf::Player player;
    set.Open(player);
    XSFLIB_CHECK(player.Seek(kFramesBeforeStop));

    // A second of the second rate: all silence, with no end to the track.
    std::vector<int16_t> frames(size_t{kRateSecond} * 2, 1);
    size_t got = 0;
    while (got < kRateSecond)
    {
        const size_t part = player.Render(frames.data() + got * 2, kRateSecond - got);
        XSFLIB_CHECK(part > 0);
        if (part == 0)
        {
            break;
        }

        got += part;
    }

    bool silent = true;
    for (int16_t sample : frames)
    {
        silent = silent && sample == 0;
    }
    XSFLIB_CHECK(silent);
    XSFLIB_CHECK(player.SampleRate() == kRateSecond);
    XSFLIB_CHECK(player.GetState() == xsflib::player::State::kPlaying);
}

void Seeks()
{
    const TestSet set(MakeCountingProgram(), {});
    usf::Player player;
    set.Open(player);

    // On, across the change of rate.
    XSFLIB_CHECK(player.Seek(kFramesBeforeChange + 50));
    XSFLIB_CHECK(player.Position() == kFramesBeforeChange + 50);
    XSFLIB_CHECK(player.SampleRate() == kRateSecond);
    CheckFrames(player, kFramesBeforeChange + 50, 10);

    // Back, across the change of rate.
    XSFLIB_CHECK(player.Seek(100));
    XSFLIB_CHECK(player.Position() == 100);
    XSFLIB_CHECK(player.SampleRate() == kRateFirst);
    CheckFrames(player, 100, 10);

    // To the place where it is.
    XSFLIB_CHECK(player.Seek(110));
    CheckFrames(player, 110, 10);
}

// The count that the program made, from the samples of its second buffer.
int RenderBusyCount(const std::vector<int16_t>& frames)
{
    return frames[size_t{kFramesPerBuffer} * 2];
}

// A buffer of 256 frames at about 32 kHz lasts 8 ms: about 190,000 instructions at the legacy rate of 2 count units for
// each. With `_enableFIFOfull`, the AI stays busy for that time. Without it, the buffer ends at the next check for
// events, within the first few reads.
void AppliesTheFlags()
{
    constexpr int kFewReads = 16;
    constexpr int kManyReads = 1000;
    std::vector<int16_t> frames(size_t{kFramesPerBuffer + 1} * 2);

    const TestSet without(MakeBusyProgram(), {});
    usf::Player quick;
    without.Open(quick);
    XSFLIB_CHECK(quick.Render(frames.data(), kFramesPerBuffer + 1) == kFramesPerBuffer + 1);
    const int quick_count = RenderBusyCount(frames);

    const TestSet with(MakeBusyProgram(), {{"_enableFIFOfull", "1"}});
    usf::Player delayed;
    with.Open(delayed);
    XSFLIB_CHECK(delayed.Render(frames.data(), kFramesPerBuffer + 1) == kFramesPerBuffer + 1);
    const int delayed_count = RenderBusyCount(frames);

    XSFLIB_CHECK(quick_count > 0 && quick_count < kFewReads);
    XSFLIB_CHECK(delayed_count > kManyReads);
}

void Failures()
{
    const TestSet set(MakeCountingProgram(), {});
    std::vector<int16_t> frames(16, 1);

    // Before Start.
    usf::Player idle;
    XSFLIB_CHECK(idle.SampleRate() == 0);
    XSFLIB_CHECK(idle.Render(frames.data(), 8) == 0);
    XSFLIB_CHECK(!idle.Start());

    // A file that isn't there.
    usf::Player missing;
    XSFLIB_CHECK(!missing.Load((set.Folder() / "Nothing.miniusf").string(), xsflib::psf::ReadWholeFile));
    XSFLIB_CHECK(!missing.Error().empty());
    XSFLIB_CHECK(!missing.Start());
    XSFLIB_CHECK(missing.SampleRate() == 0);
    XSFLIB_CHECK(missing.Render(frames.data(), 8) == 0);

    // A file with no state in its chain.
    usf::Sections sections;
    sections.rom = std::vector<usf::Chunk>{{0, MakeRom()}};
    const std::filesystem::path stateless = set.Folder() / "Stateless.usf";
    XSFLIB_CHECK(!xsflib::WriteFile(stateless, usf::WriteUsf(sections, {})));
    usf::Player empty;
    XSFLIB_CHECK(!empty.Load(stateless.string(), xsflib::psf::ReadWholeFile));
    XSFLIB_CHECK(!empty.Error().empty());
    XSFLIB_CHECK(!empty.Start());

    // A library that isn't there.
    std::filesystem::remove(set.Folder() / "Test.usflib");
    usf::Player unlinked;
    XSFLIB_CHECK(!unlinked.Load(set.MiniPath().string(), xsflib::psf::ReadWholeFile));
    XSFLIB_CHECK(!unlinked.Error().empty());
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("player",
                                    []
                                    {
                                        PlaysAChain();
                                        RateChanges();
                                        StoppedGameGivesSilence();
                                        Seeks();
                                        AppliesTheFlags();
                                        Failures();
                                    });
}
