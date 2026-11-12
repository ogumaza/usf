// SPDX-License-Identifier: MIT

// The CPU's short path against its long path. Each program runs in two systems with a set's options. A breakpoint that
// never hits keeps one system on Step. After each slice of a run, the two must hold the same state. The slices have
// many sizes, so that the runs stop inside delay slots and loops.

#include <algorithm>
#include <cstdint>
#include <functional>
#include <vector>

#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

#include "n64/system.h"
#include "rip/mips.h"
#include "test_programs.h"

namespace
{

namespace mips = usf::rip::mips;

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
using usf::n64::Cpu;
using usf::n64::System;
using usf::testing::kCode;
using usf::testing::kKseg0Address;
using usf::testing::kStatusCu0And1;
using usf::testing::SetOptions;

constexpr uint64_t kNever = 0xFFFFFFFFFFFFFFF0ull;
constexpr uint32_t kData = 0x4000;
constexpr uint32_t kCheckedMemory = 0x10000;
constexpr uint32_t kBreak = 0x0000000D;

// LUI and ORI come from the encoder. SignedAddiu is the encoder ADDIU with a signed immediate. The programs build the
// other instructions from their fields: SPECIAL instructions, and instructions with an immediate.
using mips::Lui;
using mips::Ori;

uint32_t SignedAddiu(mips::Register rt, mips::Register rs, int16_t value)
{
    return mips::Addiu(rt, rs, static_cast<uint16_t>(value));
}

constexpr uint32_t Special(uint32_t funct, uint32_t rs, uint32_t rt, uint32_t rd, uint32_t sa = 0)
{
    return (rs << 21) | (rt << 16) | (rd << 11) | (sa << 6) | funct;
}

constexpr uint32_t Immediate(uint32_t op, uint32_t rs, uint32_t rt, uint32_t value)
{
    return (op << 26) | (rs << 21) | (rt << 16) | (value & 0xFFFF);
}

// Branch offsets count instructions from the delay slot.
constexpr uint32_t Beq(uint32_t rs, uint32_t rt, int offset)
{
    return Immediate(0x04, rs, rt, static_cast<uint32_t>(offset));
}

constexpr uint32_t Bne(uint32_t rs, uint32_t rt, int offset)
{
    return Immediate(0x05, rs, rt, static_cast<uint32_t>(offset));
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

    for (const int reg : {usf::n64::kCop0Count, usf::n64::kCop0Random, usf::n64::kCop0Cause, usf::n64::kCop0Status,
                          usf::n64::kCop0Epc, usf::n64::kCop0BadVAddr, usf::n64::kCop0EntryHi, usf::n64::kCop0Context})
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
    usf::testing::WriteProgram(system, address, program);
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
            Special(0x00, 0, kT0, kV0, 4),     // SLL
            Special(0x02, 0, kT0, kV1, 4),     // SRL
            Special(0x03, 0, kT0, kA0, 4),     // SRA
            Special(0x04, kT1, kT0, kV0),      // SLLV
            Special(0x06, kT1, kT0, kV1),      // SRLV
            Special(0x07, kT1, kT0, kA0),      // SRAV
            Special(0x21, kT0, kT1, kV0),      // ADDU
            Special(0x23, kT0, kT1, kV1),      // SUBU
            Special(0x24, kT0, kT1, kA0),      // AND
            Special(0x25, kT0, kT1, kA1),      // OR
            Special(0x26, kT0, kT1, kT2),      // XOR
            Special(0x27, kT0, kT1, kT3),      // NOR
            Special(0x2A, kT0, kT1, kV0),      // SLT
            Special(0x2B, kT0, kT1, kV1),      // SLTU
            Special(0x18, kT0, kT1, 0),        // MULT
            Special(0x10, 0, 0, kA0),          // MFHI
            Special(0x12, 0, 0, kA1),          // MFLO
            Special(0x19, kT0, kT1, 0),        // MULTU
            Special(0x10, 0, 0, kT2),          // MFHI
            Special(0x12, 0, 0, kT3),          // MFLO
            Special(0x2D, kT0, kT1, kV0),      // DADDU
            Special(0x2F, kT0, kT1, kV1),      // DSUBU
            Special(0x38, 0, kT0, kA0, 7),     // DSLL
            Special(0x3A, 0, kT0, kA1, 7),     // DSRL
            Special(0x3B, 0, kT0, kT2, 7),     // DSRA
            Special(0x3C, 0, kT0, kT3, 7),     // DSLL32
            Special(0x3E, 0, kT0, kV0, 7),     // DSRL32
            Special(0x3F, 0, kT0, kV1, 7),     // DSRA32
            Immediate(0x19, kT0, kA0, 0x8000), // DADDIU
            Immediate(0x0A, kT0, kA1, 0x8000), // SLTI
            Immediate(0x0B, kT0, kT2, 0x8000), // SLTIU
            Immediate(0x0C, kT0, kT3, 0xFFFF), // ANDI
            Immediate(0x0E, kT0, kV0, 0xFFFF), // XORI
            Special(0x21, kT0, kT1, 0),        // ADDU to register 0.
            SignedAddiu(kZero, kT0, 1),        // ADDIU to register 0.
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
            Immediate(0x06, kT1, 0, 2), // BLEZ, taken.
            SignedAddiu(kV1, kV1, 1),
            SignedAddiu(kV1, kV1, 0x10),
            Immediate(0x07, kT1, 0, 2), // BGTZ, not taken.
            SignedAddiu(kV1, kV1, 2),
            SignedAddiu(kV1, kV1, 0x20),
            Immediate(0x01, kT1, 0, 2), // BLTZ, taken.
            SignedAddiu(kA0, kA0, 1),
            SignedAddiu(kA0, kA0, 0x10),
            Immediate(0x01, kT1, 1, 2), // BGEZ, not taken.
            SignedAddiu(kA0, kA0, 2),
            SignedAddiu(kA0, kA0, 0x20),
            Immediate(0x14, kT0, kT0, 2), // BEQL, taken.
            SignedAddiu(kA1, kA1, 1),
            SignedAddiu(kA1, kA1, 0x10),
            Immediate(0x15, kT0, kT0, 2), // BNEL, not taken: the slot doesn't run.
            SignedAddiu(kA1, kA1, 2),
            SignedAddiu(kA1, kA1, 0x20),
            Immediate(0x16, kT0, 0, 2), // BLEZL, not taken.
            SignedAddiu(kT2, kT2, 1),
            Immediate(0x17, kT0, 0, 2), // BGTZL, taken.
            SignedAddiu(kT2, kT2, 2),
            SignedAddiu(kT2, kT2, 0x20),
            Immediate(0x01, kT1, 2, 2), // BLTZL, taken.
            SignedAddiu(kT3, kT3, 1),
            SignedAddiu(kT3, kT3, 0x10),
            Immediate(0x01, kT1, 3, 2), // BGEZL, not taken.
            SignedAddiu(kT3, kT3, 2),
            SignedAddiu(kT3, kT3, 0x20),
            (0x03u << 26) | ((kCode + 42 * 4) >> 2), // JAL
            SignedAddiu(kAt, kZero, 7),
            SignedAddiu(kAt, kZero, 8),              // Back here from the JR below.
            (0x02u << 26) | ((kCode + 44 * 4) >> 2), // J to the JALR's set-up.
            mips::kNop,
            Special(0x08, kRa, 0, 0), // JR
            SignedAddiu(kAt, kAt, 1),
            Lui(kT0, 0x8000), // The JALR's target is the last NOP.
            Ori(kT0, kT0, kCode + 48 * 4),
            Special(0x09, kT0, 0, kV0), // JALR
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
            Immediate(0x20, kT0, kV0, 0),     // LB
            Immediate(0x21, kT0, kV1, 0),     // LH
            Immediate(0x23, kT0, kA0, 0),     // LW
            Immediate(0x24, kT0, kA1, 0),     // LBU
            Immediate(0x25, kT0, kT2, 0),     // LHU
            Immediate(0x37, kT0, kT3, 0),     // LD
            Immediate(0x28, kT0, kT1, 0x100), // SB
            Immediate(0x29, kT0, kT1, 0x102), // SH
            Immediate(0x2B, kT0, kT1, 0x104), // SW
            Immediate(0x3F, kT0, kT1, 0x108), // SD
            Immediate(0x23, kT0, 0, 0),       // LW to register 0.
            Immediate(0x23, kT0, kV0, 2),     // A misaligned LW: address error.
        },
        12, base);

    // Device registers: MI_VERSION's word by a load, and a store to MI_MASK.
    CheckProgram({Lui(kT0, 0xA430), Immediate(0x23, kT0, kV0, 4), Immediate(0x2B, kT0, 0, 0xC), mips::kNop}, 4);

    // CACHE changes nothing that the core models. The short path runs it as a NOP.
    CheckProgram(
        {Immediate(0x2F, kT0, 0x19, 0), SignedAddiu(kV0, kZero, 1), Immediate(0x2F, kT0, 0x00, 0x10), mips::kNop}, 4,
        base);

    // Code that writes the instruction after it: the new instruction must run.
    const auto patch = [](System& system)
    {
        system.GetCpu().SetGpr(kT0, kKseg0Address + kCode + 2 * 4);
        system.GetCpu().SetGpr(kT1, SignedAddiu(kV0, kZero, 0x55));
    };
    CheckProgram({Immediate(0x2B, kT0, kT1, 0), mips::kNop, SignedAddiu(kV0, kZero, 0x11), mips::kNop}, 8, patch);
}

void PageEdges()
{
    // The program crosses a page. Its branch sits in the last word of a page, so that its slot is in the next. The
    // branch skips one instruction.
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
        system.GetCpu().SetCop0(usf::n64::kCop0Status, status);
        system.GetCpu().SetGpr(kT0, kKseg0Address + kData);
        system.GetCpu().SetGpr(kT1, 0x41200000);
    };
    CheckProgram(
        {
            (0x11u << 26) | (4u << 21) | (kT1 << 16) | (3u << 11), // MTC1 to an odd register.
            (0x11u << 26) | (4u << 21) | (kT1 << 16) | (2u << 11), // MTC1 to an even register.
            (0x11u << 26) | (0u << 21) | (kV0 << 16) | (3u << 11), // MFC1 from the odd register.
            (0x11u << 26) | (0u << 21) | (kV1 << 16) | (2u << 11), // MFC1 from the even register.
            Immediate(0x31, kT0, 5, 0),                            // LWC1 to an odd register.
            Immediate(0x31, kT0, 4, 4),                            // LWC1 to an even register.
            Immediate(0x35, kT0, 7, 0),                            // LDC1 to an odd register.
            Immediate(0x39, kT0, 5, 0x100),                        // SWC1 from the odd register.
            Immediate(0x39, kT0, 4, 0x104),                        // SWC1 from the even register.
            Immediate(0x3D, kT0, 7, 0x108),                        // SDC1 from an odd register.
            (0x11u << 26) | (8u << 21) | (0u << 16) | 2u,          // BC1F, taken: the condition bit is clear.
            SignedAddiu(kA0, kZero, 1),
            SignedAddiu(kA0, kZero, 2),
            (0x11u << 26) | (8u << 21) | (1u << 16) | 2u, // BC1T, not taken.
            SignedAddiu(kA1, kZero, 1),
            SignedAddiu(kA1, kZero, 2),
            (0x11u << 26) | (8u << 21) | (3u << 16) | 2u, // BC1TL, not taken: the slot doesn't run.
            SignedAddiu(kT2, kZero, 1),
            SignedAddiu(kT2, kZero, 2),
            (0x11u << 26) | (8u << 21) | (2u << 16) | 2u, // BC1FL, taken.
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
    CheckProgram({Lui(kV0, 0x8000), Immediate(0x24, kV0, kV0, kData + 8), Beq(kV0, 0, -3), mips::kNop}, kInstructions,
                 with_event);

    // A branch to itself with a store in its delay slot.
    CheckProgram({Beq(0, 0, -1), Immediate(0x2B, kA0, kT1, 0)}, kInstructions, with_event);

    // A branch to itself with an instruction in its slot that changes a register, and an idle loop.
    CheckProgram({Beq(0, 0, -1), SignedAddiu(kV1, kV1, 1)}, kInstructions, with_event);
    CheckProgram({Beq(0, 0, -1), mips::kNop}, kInstructions, with_event);

    // A loop that counts, with the count in the delay slot and with its end in sight.
    CheckProgram({SignedAddiu(kT2, kZero, 500), SignedAddiu(kT2, kT2, -1), Bne(kT2, 0, -2), SignedAddiu(kV1, kV1, 1),
                  SignedAddiu(kV0, kZero, 9), mips::kNop},
                 kInstructions, with_event);

    // A loop that stores what it counts, and reads it back.
    CheckProgram({Immediate(0x23, kA0, kV0, 0), SignedAddiu(kV0, kV0, 1), Immediate(0x2B, kA0, kV0, 0), Beq(0, 0, -4),
                  mips::kNop},
                 kInstructions, with_event);

    // A loop that counts in one register while another stays the same.
    CheckProgram({SignedAddiu(kV0, kV0, 1), Lui(kV1, 0x1234), Beq(0, 0, -3), mips::kNop}, kInstructions, with_event);

    // A BREAK after a check point: it moves Count from that point's time. The branch and its slot run in one slice.
    CheckProgram({mips::kNop, mips::kNop, mips::kNop, Beq(0, 0, 0), mips::kNop, mips::kNop, mips::kNop, kBreak,
                  SignedAddiu(kV0, kZero, 1), mips::kNop},
                 kInstructions, with_event);

    // Registers that settle after a pass: v1 takes v0's old value.
    CheckProgram({Special(0x21, kV0, 0, kV1), Immediate(0x24, kA0, kV0, 0), Beq(0, 0, -3), mips::kNop}, kInstructions,
                 with_event);

    // A poll of a device that the short path leaves to Step: AI_LEN.
    CheckProgram({Lui(kV0, 0xA450), Immediate(0x23, kV0, kV1, 0x04), Beq(0, 0, -3), mips::kNop}, kInstructions,
                 with_event);

    // A short loop that the code leaves and enters again in the same page. Its count starts at 2 on each entry, so its
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

// A loop that waits until the half-line falls back, counts the field in v0 and waits again. t0 holds kViBase << 16.
std::vector<uint32_t> FieldWaitLoop()
{
    const uint32_t read = Immediate(0x23, kT0, kT1, kViCurrent);
    const uint32_t keep = Special(0x25, kT1, 0, kT3);
    const uint32_t fell = Special(0x2B, kT1, kT3, kT2);

    return {read, keep, read, fell, Beq(kT2, 0, -4), mips::kNop, SignedAddiu(kV0, kV0, 1), Beq(0, 0, -7), mips::kNop};
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

    const uint32_t read = Immediate(0x23, kT0, kT1, kViCurrent);
    const uint32_t keep = Special(0x25, kT1, 0, kT3);
    const uint32_t fell = Special(0x2B, kT1, kT3, kT2);

    const auto write = [](uint32_t offset)
    {
        return Immediate(0x2B, kT0, kA0, offset);
    };

    // A wait until the half-line falls back.
    XSFLIB_CHECK(CheckProgram(WithFieldWait({Lui(kT0, kViBase)}), kInstructions, nullptr, kCode, kLargestSlice) >=
                 kFields);

    // The same wait with its read in the delay slot.
    XSFLIB_CHECK(CheckProgram({Lui(kT0, kViBase), read, fell, keep, Beq(kT2, 0, -3), read, SignedAddiu(kV0, kV0, 1),
                               Beq(0, 0, -6), mips::kNop},
                              kInstructions, nullptr, kCode, kLargestSlice) >= kFields);

    // V_SYNC sets a field's length.
    XSFLIB_CHECK(CheckProgram(WithFieldWait({Lui(kT0, kViBase), SignedAddiu(kA0, kZero, 311), write(kViSync)}),
                              kInstructions, nullptr, kCode, kLargestSlice) >= kFields);

    // In the interlaced modes, bit 0 of the half-line gives the field.
    XSFLIB_CHECK(CheckProgram(WithFieldWait({Lui(kT0, kViBase), SignedAddiu(kA0, kZero, kSerrate), write(kViStatus)}),
                              kInstructions, nullptr, kCode, kLargestSlice) >= kFields);

    // A wait for one half-line, then a wait for the next.
    XSFLIB_CHECK(
        CheckProgram({Lui(kT0, kViBase), SignedAddiu(kT3, kZero, 100), read, Bne(kT1, kT3, -2), mips::kNop,
                      SignedAddiu(kV0, kV0, 1), read, Beq(kT1, kT3, -2), mips::kNop, Beq(0, 0, -8), mips::kNop},
                     kInstructions, nullptr, kCode, kLargestSlice) >= kFields);

    // A read whose value the loop drops, so that nothing changes in the registers from one half-line to the next.
    CheckProgram({Lui(kT0, kViBase), read, SignedAddiu(kT1, kZero, 5), Beq(0, 0, -3), mips::kNop}, kInstructions,
                 nullptr, kCode, kLargestSlice);

    // A BREAK moves Count from the time of the last device read.
    CheckProgram({Lui(kT0, kViBase), mips::kNop, Beq(0, 0, 0), mips::kNop, read, mips::kNop, mips::kNop, kBreak,
                  SignedAddiu(kV0, kV0, 1), mips::kNop},
                 kInstructions, nullptr, kCode, kLargestSlice);

    // A read outside KSEG1 raises what it raised before: VI_CURRENT's address with no segment bits.
    CheckProgram({Lui(kT0, 0x0440), read, Beq(0, 0, -3), mips::kNop}, 2000, nullptr, kCode, kLargestSlice);
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
        constexpr uint32_t kEret = 0x42000018;
        xsflib::StoreBe32(system.Rdram().data() + 0x180, kEret);
        system.GetCpu().SetCop0(usf::n64::kCop0Compare, 2000);

        // The timer's line (IM7, at the bit of IP7 in Cause), and interrupts on.
        constexpr uint32_t kStatus = kStatusCu0And1 | usf::n64::kStatusFr | usf::n64::kCauseIp7 | usf::n64::kStatusIe;
        system.GetCpu().SetCop0(usf::n64::kCop0Status, kStatus);
    };
    CheckProgram({Lui(kV0, 0x8000), Immediate(0x24, kV0, kV0, kData + 8), Beq(kV0, 0, -3), mips::kNop}, 60000, setup);
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
                                        Cop1(kStatusCu0And1 | usf::n64::kStatusFr); // CU1 with FR set.
                                        Cop1(kStatusCu0And1);                       // CU1 with FR clear.
                                        Cop1(usf::n64::kStatusCu0);                 // COP1 unusable: the exception.
                                        Loops();
                                        ViPolls();
                                        ViPollAtInterrupt();
                                        WaitEndsInAnInterrupt();
                                    });
}
