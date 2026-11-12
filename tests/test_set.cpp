// SPDX-License-Identifier: MIT

#include "test_set.h"

#include <array>

#include "xsflib/bytes.h"

#include "n64/save_state.h"
#include "n64/system.h"
#include "rip/mips.h"
#include "test_mips.h"
#include "test_programs.h"
#include "usf/usf_file.h"

namespace usf::testing
{
namespace
{

namespace mips = usf::rip::mips;

using mips::kS0;
using mips::kS1;
using mips::kS2;
using mips::kS3;
using mips::kS4;
using mips::kS5;
using mips::kT0;
using mips::kT1;
using mips::kT2;
using mips::kT3;
using mips::kT4;
using mips::kT6;
using mips::kT7;
using mips::kT8;
using mips::kT9;
using mips::kZero;
using usf::n64::kCop0Epc;
using usf::n64::kCop0Status;
using usf::n64::kKseg0;

constexpr uint32_t kExceptionVector = 0x00000180;
constexpr uint32_t kParameters = 0x00100000;
constexpr uint32_t kWorkArea = 0x00100100;
constexpr uint32_t kBuffers = 0x00200000; // Two buffers, 0x1000 bytes apart.
constexpr uint32_t kRomSize = 0x1000;

// The NTSC video clock. A rate is the clock over DACRATE + 1.
constexpr uint32_t kNtscVideoClock = 48681812;

// The parameters' offsets in their block.
constexpr uint16_t kDacRate = 0;
constexpr uint16_t kCpuLoops = 4;
constexpr uint16_t kRspLoops = 8;
constexpr uint16_t kStep = 12;
constexpr uint16_t kMask = 16;
constexpr uint16_t kChangeAfter = 20;
constexpr uint16_t kSecondDacRate = 24;
constexpr uint16_t kStopAfter = 28;

// Status with coprocessor 1 usable and the MI's interrupt line enabled: without interrupts, with them, and inside an
// exception.
constexpr uint32_t kStatusAsleep = 0x20000400;
constexpr uint32_t kStatusAwake = kStatusAsleep | 1;
constexpr uint32_t kStatusInException = kStatusAsleep | 2;

uint32_t Mfc0(mips::Register rt, uint32_t rd)
{
    return 0x40000000 | (uint32_t{rt} << 16) | (rd << 11);
}

uint32_t Mtc0(mips::Register rt, uint32_t rd)
{
    return 0x40800000 | (uint32_t{rt} << 16) | (rd << 11);
}

constexpr uint32_t kEret = 0x42000018;

// The FPU's instructions, on single-precision registers.
uint32_t Mtc1(mips::Register rt, uint32_t fs)
{
    return 0x44800000 | (uint32_t{rt} << 16) | (fs << 11);
}

uint32_t CvtSW(uint32_t fd, uint32_t fs)
{
    return 0x46800020 | (fs << 11) | (fd << 6);
}

uint32_t AddS(uint32_t fd, uint32_t fs, uint32_t ft)
{
    return 0x46000000 | (ft << 16) | (fs << 11) | (fd << 6);
}

uint32_t MulS(uint32_t fd, uint32_t fs, uint32_t ft)
{
    return 0x46000002 | (ft << 16) | (fs << 11) | (fd << 6);
}

// The RSP's program: a loop of vector multiplies and sums on two vectors of DMEM, and then a BREAK. The word at address
// 0 of DMEM gives the loop's count.
std::vector<uint32_t> MakeRspProgram()
{
    return {
        0x8C010000,                 // lw r1, 0(r0)
        RspLqv(1, 1),               // loop: lqv $v1[0], 16(r0)
        RspLqv(2, 2),               // lqv $v2[0], 32(r0)
        RspVector(kVmulf, 3, 1, 2), // vmulf $v3, $v1, $v2
        RspVector(kVmacf, 4, 1, 2), // vmacf $v4, $v1, $v2
        RspVector(kVmudh, 5, 3, 4), // vmudh $v5, $v3, $v4
        RspVector(kVadd, 6, 5, 1),  // vadd $v6, $v5, $v1
        RspSqv(6, 3),               // sqv $v6[0], 48(r0)
        0x2021FFFF,                 // addi r1, r1, -1
        0x1C20FFF7,                 // bgtz r1, loop
        0x00000000,                 // nop
        0x0000000D,                 // break
    };
}

// The handler of the AI's interrupt: it clears the interrupt, leaves interrupts off and returns past the idle loop.
std::vector<uint32_t> MakeExceptionHandler()
{
    mips::Assembler code(kKseg0 | kExceptionVector);
    code.Emit(mips::Sw(kZero, kS2, 0xC));
    code.LoadImmediate(kT9, kStatusInException);
    code.Emit(Mtc0(kT9, kCop0Status));
    code.Emit(Mfc0(kT8, kCop0Epc));
    code.Emit(mips::Addiu(kT8, kT8, 8));
    code.Emit(Mtc0(kT8, kCop0Epc));
    code.Emit(kEret);

    return code.Words();
}

// The parameters, the video clock that a buffer's length follows, and the AI with its interrupt.
void EmitSetUp(mips::Assembler& code)
{
    code.Emit(mips::Lui(kS3, mips::High(kKseg0 | kParameters)));
    code.Emit(mips::Addiu(kS3, kS3, mips::Low(kKseg0 | kParameters)));
    code.Emit(mips::Lui(kT0, 0xA440));
    code.Emit(mips::Addiu(kT1, kZero, 524));
    code.Emit(mips::Sw(kT1, kT0, 0x18));
    EmitAiSetup(code, mips::Lw(kT0, kS3, kDacRate));
    code.Emit(mips::Lui(kT0, 0xA430));
    code.Emit(mips::Addiu(kT1, kZero, 0x20));
    code.Emit(mips::Sw(kT1, kT0, 0xC));
    code.Emit(mips::Lui(kS4, 0xA404));
    code.Emit(mips::Addiu(kS0, kZero, 0));
    code.Emit(mips::Addiu(kS1, kZero, 0));
    code.Emit(mips::Addiu(kS5, kZero, 0));
}

// The program may stop sending buffers. It goes on running with its interrupts off.
void EmitStop(mips::Assembler& code)
{
    code.Emit(mips::Lw(kT0, kS3, kStopAfter));
    code.Emit(mips::Addiu(kT1, kS1, 1));
    const size_t go_on = code.Size();
    code.Bne(kT0, kT1, code.Here());
    code.Emit(mips::kNop);
    const uint32_t stopped = code.Here();
    code.J(stopped);
    code.Emit(mips::kNop);
    code.SetBranchTarget(go_on, code.Here());
}

// The buffer: frame i holds the ramp on the left and one more on the right.
void EmitFill(mips::Assembler& code)
{
    code.Emit(mips::Lui(kT0, mips::High(kKseg0 | kBuffers)));
    code.Emit(mips::Sll(kT1, kS5, 12));
    code.Emit(mips::Addu(kT0, kT0, kT1));
    code.Emit(mips::Lw(kT6, kS3, kMask));
    code.Emit(mips::Lw(kT7, kS3, kStep));
    code.Emit(mips::Addiu(kT1, kZero, 0));
    const uint32_t fill = code.Here();
    code.Emit(mips::And(kT2, kS0, kT6));
    code.Emit(mips::Addiu(kT3, kS0, 1));
    code.Emit(mips::And(kT3, kT3, kT6));
    code.Emit(mips::Sll(kT2, kT2, 16));
    code.Emit(mips::Or(kT2, kT2, kT3));
    code.Emit(mips::Sw(kT2, kT0, 0));
    code.Emit(mips::Addiu(kT0, kT0, 4));
    code.Emit(mips::Addu(kS0, kS0, kT7));
    code.Emit(mips::Addiu(kT1, kT1, 1));
    code.Emit(mips::Addiu(kT4, kZero, kTestBufferFrames));
    code.Bne(kT1, kT4, fill);
    code.Emit(mips::kNop);
}

// The CPU's work loop.
void EmitWorkLoop(mips::Assembler& code)
{
    code.Emit(mips::Lw(kT0, kS3, kCpuLoops));
    const size_t skip_work = code.Size();
    code.Beq(kT0, kZero, code.Here());
    code.Emit(mips::kNop);
    code.Emit(mips::Lui(kT1, mips::High(kKseg0 | kWorkArea)));
    code.Emit(mips::Addiu(kT1, kT1, mips::Low(kKseg0 | kWorkArea)));
    const uint32_t work = code.Here();
    code.Emit(mips::Lw(kT2, kT1, 0));
    code.Emit(mips::Addu(kT3, kT2, kS0));
    code.Emit(mips::Sll(kT4, kT3, 3));
    code.Emit(mips::Sw(kT4, kT1, 4));
    code.Emit(Mtc1(kT3, 0));
    code.Emit(CvtSW(0, 0));
    code.Emit(AddS(2, 0, 0));
    code.Emit(MulS(4, 2, 0));
    code.Emit(mips::Addiu(kT0, kT0, 0xFFFF));
    code.Bne(kT0, kZero, work);
    code.Emit(mips::kNop);
    code.SetBranchTarget(skip_work, code.Here());
}

// The RSP's task: its loop count, an audio task's type, the start of IMEM, and a clear of halt, broke and the
// interrupt. The task runs to its BREAK.
void EmitRspTask(mips::Assembler& code)
{
    code.Emit(mips::Lw(kT0, kS3, kRspLoops));
    const size_t skip_task = code.Size();
    code.Beq(kT0, kZero, code.Here());
    code.Emit(mips::kNop);
    code.Emit(mips::Lui(kT1, 0xA400));
    code.Emit(mips::Sw(kT0, kT1, 0));
    code.Emit(mips::Addiu(kT2, kZero, 2));
    code.Emit(mips::Sw(kT2, kT1, 0xFC0));
    code.Emit(mips::Lui(kT2, 0xA408));
    code.Emit(mips::Sw(kZero, kT2, 0));
    code.Emit(mips::Addiu(kT2, kZero, 0xD));
    code.Emit(mips::Sw(kT2, kS4, 0x10));
    const uint32_t wait_rsp = code.Here();
    code.Emit(mips::Lw(kT2, kS4, 0x10));
    code.Emit(mips::Andi(kT2, kT2, 1));
    code.Beq(kT2, kZero, wait_rsp);
    code.Emit(mips::kNop);
    code.SetBranchTarget(skip_task, code.Here());
}

// Hand the buffer to the AI.
void EmitHandOff(mips::Assembler& code)
{
    code.Emit(mips::Lui(kT0, kBuffers >> 16));
    code.Emit(mips::Sll(kT1, kS5, 12));
    code.Emit(mips::Addu(kT0, kT0, kT1));
    code.Emit(mips::Sw(kT0, kS2, 0x0));
    code.Emit(mips::Addiu(kT0, kZero, kTestBufferFrames * 4));
    code.Emit(mips::Sw(kT0, kS2, 0x4));
    code.Emit(mips::Addiu(kS5, kS5, 1));
    code.Emit(mips::Andi(kS5, kS5, 1));
    code.Emit(mips::Addiu(kS1, kS1, 1));
}

// The rate changes after the buffers that the parameters say. A DACRATE takes effect at the next buffer.
void EmitRateChange(mips::Assembler& code)
{
    code.Emit(mips::Lw(kT0, kS3, kChangeAfter));
    const size_t same_rate = code.Size();
    code.Bne(kS1, kT0, code.Here());
    code.Emit(mips::kNop);
    code.Emit(mips::Lw(kT0, kS3, kSecondDacRate));
    code.Emit(mips::Sw(kT0, kS2, 0x10));
    code.SetBranchTarget(same_rate, code.Here());
}

// With room in the AI, the next buffer starts at once. Otherwise the program sleeps until the AI's interrupt. The
// handler returns to the jump after the idle loop.
void EmitIdle(mips::Assembler& code, uint32_t loop)
{
    code.Emit(mips::Lw(kT0, kS2, 0xC));
    code.Emit(mips::Srl(kT0, kT0, 31));
    code.Beq(kT0, kZero, loop);
    code.Emit(mips::kNop);
    code.LoadImmediate(kT0, kStatusAwake);
    code.Emit(Mtc0(kT0, kCop0Status));
    const uint32_t idle = code.Here();
    code.J(idle);
    code.Emit(mips::kNop);
    code.J(loop);
    code.Emit(mips::kNop);
}

// The CPU's program (see test_set.h). s0 holds the ramp, s1 the buffers so far, s2 the AI's registers, s3 the
// parameters, s4 the SP's registers and s5 the buffer in use. The handler overwrites only t8 and t9.
std::vector<uint32_t> MakeCpuProgram()
{
    mips::Assembler code(kKseg0 | kCode);
    EmitSetUp(code);

    const uint32_t loop = code.Here();
    EmitStop(code);
    EmitFill(code);
    EmitWorkLoop(code);
    EmitRspTask(code);
    EmitHandOff(code);
    EmitRateChange(code);
    EmitIdle(code, loop);

    return code.Words();
}

// The state: the programs in memory, and the CPU about to start. The ROM's bytes in LE word order are the file's.
std::vector<uint8_t> MakeState()
{
    usf::n64::System system(LegacyOptions());
    system.LoadRom(TestRom(kRomSize));
    WriteProgram(system, kExceptionVector, MakeExceptionHandler());
    StartProgram(system, MakeCpuProgram(), kStatusAsleep);
    WriteProgram(system, usf::n64::kSpImem, MakeRspProgram());

    return system.SaveState();
}

} // namespace

uint32_t TestRate(uint32_t dac_rate)
{
    return kNtscVideoClock / (dac_rate + 1);
}

int16_t TestLeft(const TestTrack& track, uint64_t frame)
{
    return static_cast<int16_t>(static_cast<uint32_t>(frame * track.step) & track.mask);
}

int16_t TestRight(const TestTrack& track, uint64_t frame)
{
    return static_cast<int16_t>((static_cast<uint32_t>(frame * track.step) + 1) & track.mask);
}

std::vector<uint8_t> MakeTestLibrary(const xsflib::psf::Tags& tags)
{
    // The ROM section is in LE word order: console byte A is at A XOR 3.
    const std::vector<uint8_t> rom = TestRom(kRomSize);
    std::vector<uint8_t> words(rom.size());
    usf::n64::CopyToLeWords(rom.data(), words.data(), rom.size());

    usf::Sections sections;
    sections.rom = std::vector<usf::Chunk>{{0, words}};
    sections.state = std::vector<usf::Chunk>{{0, MakeState()}};

    return usf::WriteUsf(sections, tags);
}

std::vector<uint8_t> MakeTestMini(const TestTrack& track, const xsflib::psf::Tags& tags)
{
    const std::array<uint32_t, 8> values = {
        track.dac_rate, track.cpu_loops,    track.rsp_loops,       track.step,
        track.mask,     track.change_after, track.second_dac_rate, track.stop_after};
    std::vector<uint8_t> block(4 * values.size());
    for (size_t i = 0; i < values.size(); i++)
    {
        xsflib::StoreLe32(block.data() + 4 * i, values[i]);
    }

    usf::Sections sections;
    sections.state = std::vector<usf::Chunk>{{usf::n64::kStateRdramOffset + kParameters, block}};

    return usf::WriteUsf(sections, tags);
}

} // namespace usf::testing
