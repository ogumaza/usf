// SPDX-License-Identifier: MIT

// The CPU's short path against its long path. Each program runs in two systems with a set's options. A breakpoint that
// never hits keeps one system on Step. After each slice of a run, the two must have the same state. The slices have
// many sizes. The runs then stop inside delay slots and loops.

#include <algorithm>
#include <cstdint>
#include <functional>
#include <vector>

#include "ultralib/machine/system.h"
#include "ultralib/testing/test_programs.h"
#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

#include "rip/mips.h"
#include "test_mips.h"

namespace
{

namespace mips = usf::rip::mips;

using mips::IType;
using mips::kA0;
using mips::kA1;
using mips::kAt;
using mips::kRa;
using mips::kT0;
using mips::kT1;
using mips::kT2;
using mips::kT3;
using mips::kV0;
using mips::kV1;
using mips::kZero;
using mips::RType;
using ultralib::machine::Cpu;
using ultralib::machine::System;
using ultralib::testing::kCode;
using ultralib::testing::kKseg0Address;
using ultralib::testing::kStatusCu0And1;
using ultralib::testing::SetOptions;
using usf::testing::kBreak;

constexpr uint64_t kNever = 0xFFFFFFFFFFFFFFF0ull;
constexpr uint32_t kData = 0x4000;
constexpr uint32_t kCheckedMemory = 0x10000;

// LUI and ORI come from the encoder. SignedAddiu is the encoder ADDIU with a signed immediate. The programs build the
// other instructions from their fields, with the encoder's IType and RType.
using mips::Lui;
using mips::Ori;

uint32_t SignedAddiu(mips::Register rt, mips::Register rs, int16_t value)
{
    return mips::Addiu(rt, rs, static_cast<uint16_t>(value));
}

// Branch offsets count instructions from the delay slot.
constexpr uint32_t Beq(uint32_t rs, uint32_t rt, int offset)
{
    return IType(0x04, rs, rt, static_cast<uint16_t>(offset));
}

constexpr uint32_t Bne(uint32_t rs, uint32_t rt, int offset)
{
    return IType(0x05, rs, rt, static_cast<uint16_t>(offset));
}

void CheckSameState(const System& fast, const System& slow)
{
    const Cpu& a = fast.GetCpu();
    const Cpu& b = slow.GetCpu();
    XSFLIB_CHECK(a.Pc() == b.Pc());
    for (int i = 0; i < 32; i++)
    {
        XSFLIB_CHECK(a.Gpr(i) == b.Gpr(i));
        XSFLIB_CHECK(a.Fpr(i) == b.Fpr(i));
    }

    for (const int reg : {ultralib::machine::kCop0Count, ultralib::machine::kCop0Random, ultralib::machine::kCop0Cause,
                          ultralib::machine::kCop0Status, ultralib::machine::kCop0Epc, ultralib::machine::kCop0BadVAddr,
                          ultralib::machine::kCop0EntryHi, ultralib::machine::kCop0Context})
    {
        XSFLIB_CHECK(a.Cop0(reg) == b.Cop0(reg));
    }

    XSFLIB_CHECK(fast.Now() == slow.Now());
    const auto memory_a = fast.Rdram().first(kCheckedMemory);
    const auto memory_b = slow.Rdram().first(kCheckedMemory);
    XSFLIB_CHECK(std::equal(memory_a.begin(), memory_a.end(), memory_b.begin()));
}

// Puts `program` at `address` and moves the CPU to it.
void LoadProgram(System& system, uint32_t address, const std::vector<uint32_t>& program)
{
    ultralib::testing::WriteProgram(system, address, program);
    system.GetCpu().SetPc(kKseg0Address + address);
}

// Runs a program from `address` in both systems, for about `instructions` instructions in all. No slice runs more than
// `largest_slice` instructions. Returns v0 at the end.
uint64_t CheckProgram(const std::vector<uint32_t>& program, uint64_t instructions,
                      const std::function<void(System&)>& setup = nullptr, uint32_t address = kCode,
                      uint64_t largest_slice = ~uint64_t{0})
{
    System fast(SetOptions(true));
    System slow(SetOptions(true));
    slow.AddBreakpoint(kNever);
    for (System* system : {&fast, &slow})
    {
        // Data that the loads read: bytes with the sign bit set, and zeros from kData + 8.
        for (uint32_t i = 0; i < 8; i++)
        {
            system->Rdram()[kData + i] = static_cast<uint8_t>(0x81 + i * 0x11);
        }

        if (setup)
        {
            setup(*system);
        }

        LoadProgram(*system, address, program);
    }

    const uint64_t ticks = fast.TicksPerInstruction();
    uint64_t run = 0;
    uint64_t slice = 1;
    while (run < instructions)
    {
        fast.Run(slice * ticks);
        slow.Run(slice * ticks);
        CheckSameState(fast, slow);
        run += slice;
        slice = std::min(slice < 40 ? slice + 1 : slice * 3, largest_slice);
    }

    return fast.GetCpu().Gpr(kV0);
}

void Arithmetic()
{
    CheckProgram(
        {
            Lui(kT0, 0x8765),
            Ori(kT0, kT0, 0x4321),
            SignedAddiu(kT1, kZero, -3),
            RType(0, kT0, kV0, 4, 0x00),   // SLL
            RType(0, kT0, kV1, 4, 0x02),   // SRL
            RType(0, kT0, kA0, 4, 0x03),   // SRA
            RType(kT1, kT0, kV0, 0, 0x04), // SLLV
            RType(kT1, kT0, kV1, 0, 0x06), // SRLV
            RType(kT1, kT0, kA0, 0, 0x07), // SRAV
            RType(kT0, kT1, kV0, 0, 0x21), // ADDU
            RType(kT0, kT1, kV1, 0, 0x23), // SUBU
            RType(kT0, kT1, kA0, 0, 0x24), // AND
            RType(kT0, kT1, kA1, 0, 0x25), // OR
            RType(kT0, kT1, kT2, 0, 0x26), // XOR
            RType(kT0, kT1, kT3, 0, 0x27), // NOR
            RType(kT0, kT1, kV0, 0, 0x2A), // SLT
            RType(kT0, kT1, kV1, 0, 0x2B), // SLTU
            RType(kT0, kT1, 0, 0, 0x18),   // MULT
            RType(0, 0, kA0, 0, 0x10),     // MFHI
            RType(0, 0, kA1, 0, 0x12),     // MFLO
            RType(kT0, kT1, 0, 0, 0x19),   // MULTU
            RType(0, 0, kT2, 0, 0x10),     // MFHI
            RType(0, 0, kT3, 0, 0x12),     // MFLO
            RType(kT0, kT1, kV0, 0, 0x2D), // DADDU
            RType(kT0, kT1, kV1, 0, 0x2F), // DSUBU
            RType(0, kT0, kA0, 7, 0x38),   // DSLL
            RType(0, kT0, kA1, 7, 0x3A),   // DSRL
            RType(0, kT0, kT2, 7, 0x3B),   // DSRA
            RType(0, kT0, kT3, 7, 0x3C),   // DSLL32
            RType(0, kT0, kV0, 7, 0x3E),   // DSRL32
            RType(0, kT0, kV1, 7, 0x3F),   // DSRA32
            IType(0x19, kT0, kA0, 0x8000), // DADDIU
            IType(0x0A, kT0, kA1, 0x8000), // SLTI
            IType(0x0B, kT0, kT2, 0x8000), // SLTIU
            IType(0x0C, kT0, kT3, 0xFFFF), // ANDI
            IType(0x0E, kT0, kV0, 0xFFFF), // XORI
            RType(kT0, kT1, 0, 0, 0x21),   // ADDU to register 0.
            SignedAddiu(kZero, kT0, 1),    // ADDIU to register 0.
            mips::kNop,
        },
        60);
}

void Branches()
{
    // Each branch is followed by a delay slot that counts, and an instruction that a taken branch skips. The offsets of
    // 2 jump over that instruction.
    CheckProgram(
        {
            SignedAddiu(kT0, kZero, 1),
            SignedAddiu(kT1, kZero, -1),
            Beq(kT0, kT0, 2), // Taken.
            SignedAddiu(kV0, kV0, 1),
            SignedAddiu(kV0, kV0, 0x10),
            Bne(kT0, kT0, 2), // Not taken.
            SignedAddiu(kV0, kV0, 2),
            SignedAddiu(kV0, kV0, 0x20),
            IType(0x06, kT1, 0, 2), // BLEZ, taken.
            SignedAddiu(kV1, kV1, 1),
            SignedAddiu(kV1, kV1, 0x10),
            IType(0x07, kT1, 0, 2), // BGTZ, not taken.
            SignedAddiu(kV1, kV1, 2),
            SignedAddiu(kV1, kV1, 0x20),
            IType(0x01, kT1, 0, 2), // BLTZ, taken.
            SignedAddiu(kA0, kA0, 1),
            SignedAddiu(kA0, kA0, 0x10),
            IType(0x01, kT1, 1, 2), // BGEZ, not taken.
            SignedAddiu(kA0, kA0, 2),
            SignedAddiu(kA0, kA0, 0x20),
            IType(0x14, kT0, kT0, 2), // BEQL, taken.
            SignedAddiu(kA1, kA1, 1),
            SignedAddiu(kA1, kA1, 0x10),
            IType(0x15, kT0, kT0, 2), // BNEL, not taken: the slot doesn't run.
            SignedAddiu(kA1, kA1, 2),
            SignedAddiu(kA1, kA1, 0x20),
            IType(0x16, kT0, 0, 2), // BLEZL, not taken.
            SignedAddiu(kT2, kT2, 1),
            IType(0x17, kT0, 0, 2), // BGTZL, taken.
            SignedAddiu(kT2, kT2, 2),
            SignedAddiu(kT2, kT2, 0x20),
            IType(0x01, kT1, 2, 2), // BLTZL, taken.
            SignedAddiu(kT3, kT3, 1),
            SignedAddiu(kT3, kT3, 0x10),
            IType(0x01, kT1, 3, 2), // BGEZL, not taken.
            SignedAddiu(kT3, kT3, 2),
            SignedAddiu(kT3, kT3, 0x20),
            (0x03u << 26) | ((kCode + 42 * 4) >> 2), // JAL
            SignedAddiu(kAt, kZero, 7),
            SignedAddiu(kAt, kZero, 8),              // Back here from the JR below.
            (0x02u << 26) | ((kCode + 44 * 4) >> 2), // J to the JALR's set-up.
            mips::kNop,
            RType(kRa, 0, 0, 0, 0x08), // JR
            SignedAddiu(kAt, kAt, 1),
            Lui(kT0, 0x8000), // The JALR's target is the last NOP.
            Ori(kT0, kT0, kCode + 48 * 4),
            RType(kT0, 0, kV0, 0, 0x09), // JALR
            SignedAddiu(kAt, kAt, 0x10),
            mips::kNop,
        },
        80);
}

void LoadsAndStores()
{
    const auto base = [](System& system)
    {
        system.GetCpu().SetGpr(kT0, kKseg0Address + kData);
        system.GetCpu().SetGpr(kT1, 0x1122334455667788ull);
    };
    CheckProgram(
        {
            IType(0x20, kT0, kV0, 0),     // LB
            IType(0x21, kT0, kV1, 0),     // LH
            IType(0x23, kT0, kA0, 0),     // LW
            IType(0x24, kT0, kA1, 0),     // LBU
            IType(0x25, kT0, kT2, 0),     // LHU
            IType(0x37, kT0, kT3, 0),     // LD
            IType(0x28, kT0, kT1, 0x100), // SB
            IType(0x29, kT0, kT1, 0x102), // SH
            IType(0x2B, kT0, kT1, 0x104), // SW
            IType(0x3F, kT0, kT1, 0x108), // SD
            IType(0x23, kT0, 0, 0),       // LW to register 0.
            IType(0x23, kT0, kV0, 2),     // A misaligned LW: address error.
        },
        12, base);

    // Device registers: MI_VERSION's word by a load, and a store to MI_MASK.
    CheckProgram({Lui(kT0, 0xA430), IType(0x23, kT0, kV0, 4), IType(0x2B, kT0, 0, 0xC), mips::kNop}, 4);

    // CACHE changes nothing that the core models. The short path runs it as a NOP.
    CheckProgram({IType(0x2F, kT0, 0x19, 0), SignedAddiu(kV0, kZero, 1), IType(0x2F, kT0, 0x00, 0x10), mips::kNop}, 4,
                 base);

    // Code that writes the instruction after it: the new instruction must run.
    const auto patch = [](System& system)
    {
        system.GetCpu().SetGpr(kT0, kKseg0Address + kCode + 2 * 4);
        system.GetCpu().SetGpr(kT1, SignedAddiu(kV0, kZero, 0x55));
    };
    CheckProgram({IType(0x2B, kT0, kT1, 0), mips::kNop, SignedAddiu(kV0, kZero, 0x11), mips::kNop}, 8, patch);
}

void PageEdges()
{
    // The program crosses a page. Its branch is in the last word of a page, and its slot in the next. The branch skips
    // one instruction.
    constexpr uint32_t kPageEnd = 0x2000;
    CheckProgram(
        {
            SignedAddiu(kV0, kZero, 1),
            SignedAddiu(kV0, kV0, 1),
            SignedAddiu(kV0, kV0, 1),
            Beq(0, 0, 2),
            SignedAddiu(kV1, kZero, 2),
            SignedAddiu(kV1, kZero, 3),
            SignedAddiu(kA0, kZero, 4),
        },
        12, nullptr, kPageEnd - 4 * 4);
}

void Cop1(uint32_t status)
{
    const auto setup = [status](System& system)
    {
        system.GetCpu().SetCop0(ultralib::machine::kCop0Status, status);
        system.GetCpu().SetGpr(kT0, kKseg0Address + kData);
        system.GetCpu().SetGpr(kT1, 0x41200000);
    };
    CheckProgram(
        {
            usf::testing::Mtc1(kT1, 3), // MTC1 to an odd register.
            usf::testing::Mtc1(kT1, 2), // MTC1 to an even register.
            usf::testing::Mfc1(kV0, 3), // MFC1 from the odd register.
            usf::testing::Mfc1(kV1, 2), // MFC1 from the even register.
            IType(0x31, kT0, 5, 0),     // LWC1 to an odd register.
            IType(0x31, kT0, 4, 4),     // LWC1 to an even register.
            IType(0x35, kT0, 7, 0),     // LDC1 to an odd register.
            IType(0x39, kT0, 5, 0x100), // SWC1 from the odd register.
            IType(0x39, kT0, 4, 0x104), // SWC1 from the even register.
            IType(0x3D, kT0, 7, 0x108), // SDC1 from an odd register.
            IType(0x11, 8, 0, 2),       // BC1F, taken: the condition bit is clear.
            SignedAddiu(kA0, kZero, 1),
            SignedAddiu(kA0, kZero, 2),
            IType(0x11, 8, 1, 2), // BC1T, not taken.
            SignedAddiu(kA1, kZero, 1),
            SignedAddiu(kA1, kZero, 2),
            IType(0x11, 8, 3, 2), // BC1TL, not taken: the slot doesn't run.
            SignedAddiu(kT2, kZero, 1),
            SignedAddiu(kT2, kZero, 2),
            IType(0x11, 8, 2, 2), // BC1FL, taken.
            SignedAddiu(kT3, kZero, 1),
            SignedAddiu(kT3, kZero, 2),
            mips::kNop,
        },
        30, setup);
}

// The waits that the short path skips, and the loops that it mustn't skip. Events fall inside each run.
void Loops()
{
    constexpr uint64_t kInstructions = 60000;
    const auto with_event = [](System& system)
    {
        system.Schedule(System::Event::kVi, 4001);
        system.GetCpu().SetGpr(kT1, 0x1234);
        system.GetCpu().SetGpr(kA0, kKseg0Address + kData + 8);
    };

    // A poll of memory that never changes: lui, lbu, branch, a NOP.
    CheckProgram({Lui(kV0, 0x8000), IType(0x24, kV0, kV0, kData + 8), Beq(kV0, 0, -3), mips::kNop}, kInstructions,
                 with_event);

    // A branch to itself with a store in its delay slot.
    CheckProgram({Beq(0, 0, -1), IType(0x2B, kA0, kT1, 0)}, kInstructions, with_event);

    // A branch to itself with an instruction in its slot that changes a register, and an idle loop.
    CheckProgram({Beq(0, 0, -1), SignedAddiu(kV1, kV1, 1)}, kInstructions, with_event);
    CheckProgram({Beq(0, 0, -1), mips::kNop}, kInstructions, with_event);

    // A loop that counts, with the count in the delay slot and with its end in sight.
    CheckProgram({SignedAddiu(kT2, kZero, 500), SignedAddiu(kT2, kT2, -1), Bne(kT2, 0, -2), SignedAddiu(kV1, kV1, 1),
                  SignedAddiu(kV0, kZero, 9), mips::kNop},
                 kInstructions, with_event);

    // A loop that stores what it counts, and reads it back.
    CheckProgram(
        {IType(0x23, kA0, kV0, 0), SignedAddiu(kV0, kV0, 1), IType(0x2B, kA0, kV0, 0), Beq(0, 0, -4), mips::kNop},
        kInstructions, with_event);

    // A loop that counts in one register while another stays the same.
    CheckProgram({SignedAddiu(kV0, kV0, 1), Lui(kV1, 0x1234), Beq(0, 0, -3), mips::kNop}, kInstructions, with_event);

    // A BREAK after a check point: it moves Count from that point's time. The branch and its slot run in one slice.
    CheckProgram({mips::kNop, mips::kNop, mips::kNop, Beq(0, 0, 0), mips::kNop, mips::kNop, mips::kNop, kBreak,
                  SignedAddiu(kV0, kZero, 1), mips::kNop},
                 kInstructions, with_event);

    // Registers that settle after a pass: v1 takes v0's old value.
    CheckProgram({RType(kV0, 0, kV1, 0, 0x21), IType(0x24, kA0, kV0, 0), Beq(0, 0, -3), mips::kNop}, kInstructions,
                 with_event);

    // A poll of a device that the short path leaves to Step: AI_LEN.
    CheckProgram({Lui(kV0, 0xA450), IType(0x23, kV0, kV1, 0x04), Beq(0, 0, -3), mips::kNop}, kInstructions, with_event);

    // A short loop that the code leaves and enters again in the same page. Its count starts at 2 on each entry. Its
    // branch is taken in the first pass only. That pass leaves the registers as the same pass of the entry before did.
    // The code between the entries counts in v0, and its branch back is too long for a wait.
    CheckProgram({SignedAddiu(kT0, kZero, 2), SignedAddiu(kT0, kT0, -1), Bne(kT0, 0, -2), mips::kNop,
                  SignedAddiu(kV0, kV0, 1), mips::kNop, mips::kNop, mips::kNop, mips::kNop, Beq(0, 0, -10), mips::kNop},
                 kInstructions, with_event);
}

// The VI's registers through KSEG1, for the programs that poll VI_CURRENT.
constexpr uint16_t kViBase = 0xA440;
constexpr uint32_t kViCurrent = 0x10;
constexpr uint32_t kViStatus = 0x00;
constexpr uint32_t kViSync = 0x18;
constexpr uint32_t kSerrate = 0x40;

// The steps of a wait for the half-line to fall back, with t0 holding kViBase << 16: an LW of VI_CURRENT into t1, an OR
// that keeps t1 in t3, and an SLTU that sets t2 when t1 is below t3.
constexpr uint32_t kReadHalfLine = IType(0x23, kT0, kT1, kViCurrent);
constexpr uint32_t kKeepHalfLine = RType(kT1, 0, kT3, 0, 0x25);
constexpr uint32_t kHalfLineFell = RType(kT1, kT3, kT2, 0, 0x2B);

// A loop that waits until the half-line falls back, counts the field in v0 and waits again.
std::vector<uint32_t> FieldWaitLoop()
{
    return {kReadHalfLine,
            kKeepHalfLine,
            kReadHalfLine,
            kHalfLineFell,
            Beq(kT2, 0, -4),
            mips::kNop,
            SignedAddiu(kV0, kV0, 1),
            Beq(0, 0, -7),
            mips::kNop};
}

// `prefix`, then FieldWaitLoop().
std::vector<uint32_t> WithFieldWait(std::vector<uint32_t> prefix)
{
    const std::vector<uint32_t> loop = FieldWaitLoop();
    prefix.insert(prefix.end(), loop.begin(), loop.end());

    return prefix;
}

// Polls of VI_CURRENT. The short path moves the clock over them one half-line at a time. Each run lasts several fields,
// and many slices stop at different points of a half-line. Each program that waits for a field counts them in v0.
void ViPolls()
{
    constexpr uint64_t kInstructions = 2000000;
    constexpr uint64_t kLargestSlice = 7919;
    constexpr uint64_t kFields = 3;

    const auto write = [](uint16_t offset)
    {
        return IType(0x2B, kT0, kA0, offset);
    };

    // A wait until the half-line falls back.
    XSFLIB_CHECK(CheckProgram(WithFieldWait({Lui(kT0, kViBase)}), kInstructions, nullptr, kCode, kLargestSlice) >=
                 kFields);

    // The same wait with its read in the delay slot.
    XSFLIB_CHECK(CheckProgram({Lui(kT0, kViBase), kReadHalfLine, kHalfLineFell, kKeepHalfLine, Beq(kT2, 0, -3),
                               kReadHalfLine, SignedAddiu(kV0, kV0, 1), Beq(0, 0, -6), mips::kNop},
                              kInstructions, nullptr, kCode, kLargestSlice) >= kFields);

    // V_SYNC sets a field's length.
    XSFLIB_CHECK(CheckProgram(WithFieldWait({Lui(kT0, kViBase), SignedAddiu(kA0, kZero, 311), write(kViSync)}),
                              kInstructions, nullptr, kCode, kLargestSlice) >= kFields);

    // In the interlaced modes, bit 0 of the half-line gives the field.
    XSFLIB_CHECK(CheckProgram(WithFieldWait({Lui(kT0, kViBase), SignedAddiu(kA0, kZero, kSerrate), write(kViStatus)}),
                              kInstructions, nullptr, kCode, kLargestSlice) >= kFields);

    // A wait for one half-line, then a wait for the next.
    XSFLIB_CHECK(CheckProgram({Lui(kT0, kViBase), SignedAddiu(kT3, kZero, 100), kReadHalfLine, Bne(kT1, kT3, -2),
                               mips::kNop, SignedAddiu(kV0, kV0, 1), kReadHalfLine, Beq(kT1, kT3, -2), mips::kNop,
                               Beq(0, 0, -8), mips::kNop},
                              kInstructions, nullptr, kCode, kLargestSlice) >= kFields);

    // A read whose value the loop drops. Nothing then changes in the registers from one half-line to the next.
    CheckProgram({Lui(kT0, kViBase), kReadHalfLine, SignedAddiu(kT1, kZero, 5), Beq(0, 0, -3), mips::kNop},
                 kInstructions, nullptr, kCode, kLargestSlice);

    // A BREAK moves Count from the time of the last device read.
    CheckProgram({Lui(kT0, kViBase), mips::kNop, Beq(0, 0, 0), mips::kNop, kReadHalfLine, mips::kNop, mips::kNop,
                  kBreak, SignedAddiu(kV0, kV0, 1), mips::kNop},
                 kInstructions, nullptr, kCode, kLargestSlice);

    // A read outside KSEG1 raises what it raised before: VI_CURRENT's address with no segment bits.
    CheckProgram({Lui(kT0, 0x0440), kReadHalfLine, Beq(0, 0, -3), mips::kNop}, 2000, nullptr, kCode, kLargestSlice);
}

// A read after the interrupt's time, in the pass during which it falls, already gets the next field's half-line. The
// loop then ends one pass sooner. Only a run that stops just then can show that. These runs stop at each instruction
// just after the first interrupt, with the loop's passes at each phase against it.
void ViPollAtInterrupt()
{
    constexpr int kMostNops = 4;
    constexpr uint64_t kMostExtra = 12;

    const std::vector<uint32_t> wait = WithFieldWait({Lui(kT0, kViBase)});
    for (int nops = 0; nops <= kMostNops; nops++)
    {
        std::vector<uint32_t> program(static_cast<size_t>(nops), mips::kNop);
        program.insert(program.end(), wait.begin(), wait.end());
        for (uint64_t extra = 0; extra <= kMostExtra; extra++)
        {
            System fast(SetOptions(true));
            System slow(SetOptions(true));
            slow.AddBreakpoint(kNever);
            const uint64_t ticks = fast.TicksPerInstruction();
            const uint64_t run = fast.NextEvent() + extra * ticks;
            for (System* system : {&fast, &slow})
            {
                LoadProgram(*system, kCode, program);
                system->Run(run);
            }

            CheckSameState(fast, slow);
        }
    }
}

// A timer interrupt ends a wait at the same point on both paths. Its handler at the vector is a return.
void WaitEndsInAnInterrupt()
{
    const auto setup = [](System& system)
    {
        xsflib::StoreBe32(system.Rdram().data() + 0x180, usf::testing::kEret);
        system.GetCpu().SetCop0(ultralib::machine::kCop0Compare, 2000);

        // The timer's line (IM7, at the bit of IP7 in Cause), and interrupts on.
        constexpr uint32_t kStatus =
            kStatusCu0And1 | ultralib::machine::kStatusFr | ultralib::machine::kCauseIp7 | ultralib::machine::kStatusIe;
        system.GetCpu().SetCop0(ultralib::machine::kCop0Status, kStatus);
    };
    CheckProgram({Lui(kV0, 0x8000), IType(0x24, kV0, kV0, kData + 8), Beq(kV0, 0, -3), mips::kNop}, 60000, setup);
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("fast_path",
                                    []
                                    {
                                        Arithmetic();
                                        Branches();
                                        LoadsAndStores();
                                        PageEdges();
                                        Cop1(kStatusCu0And1 | ultralib::machine::kStatusFr); // CU1 with FR set.
                                        Cop1(kStatusCu0And1);                                // CU1 with FR clear.
                                        Cop1(ultralib::machine::kStatusCu0); // COP1 unusable: the exception.
                                        Loops();
                                        ViPolls();
                                        ViPollAtInterrupt();
                                        WaitEndsInAnInterrupt();
                                    });
}
