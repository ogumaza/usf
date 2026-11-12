// SPDX-License-Identifier: MIT

// Short programs under the options that a USF set runs with. Each check follows a rule of the legacy players: BREAK,
// the stale IP2 of a state and the AI's latch, and the tables of TLB pages.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

#include "n64/constants.h"
#include "n64/save_state.h"
#include "n64/system.h"
#include "test_programs.h"

namespace
{

using usf::n64::Cpu;
using usf::n64::System;

using usf::testing::kCode;
using usf::testing::kCodeAddress;
using usf::testing::RunProgram;
using usf::testing::SetOptions;
using usf::testing::WriteProgram;

constexpr uint32_t kMiIntr = 0x04300008;
constexpr uint32_t kMiMask = 0x0430000C;
constexpr uint32_t kAiStatus = 0x0450000C;

constexpr uint32_t kNop = 0x00000000;
constexpr uint32_t kBreak = 0x0000000D;
constexpr uint32_t kBranchOverOne = 0x10000001;   // beq zero, zero, +1
constexpr uint32_t kBranchBackTwo = 0x1000FFFE;   // beq zero, zero, -2
constexpr uint32_t kSetT2 = 0x240A0001;           // addiu t2, zero, 1
constexpr uint32_t kStoreT1 = 0xAD090000;         // sw t1, 0(t0)
constexpr uint32_t kLoadT3 = 0x8D0B0000;          // lw t3, 0(t0)
constexpr uint32_t kLuiT0Mi = 0x3C08A430;         // lui t0, 0xA430
constexpr uint32_t kLoadMiIntr = 0x8D090008;      // lw t1, 8(t0)
constexpr uint32_t kTlbWriteIndexed = 0x42000002; // tlbwi

constexpr uint32_t kCauseCodeShift = 2;
constexpr uint32_t kCauseCodeMask = 0x1F;
constexpr uint32_t kCodeTlbLoad = 2;
constexpr uint32_t kCodeBreakpoint = 9;

constexpr uint32_t kStatusLine2 = 1u << 10; // IM2: allows CPU interrupt line 2.

constexpr uint64_t kGeneralVector = 0xFFFFFFFF80000180ull;
constexpr uint64_t kRefillVector = usf::testing::kKseg0Address;

constexpr size_t kTlbEntries = 32;
constexpr size_t kTlbEntrySize = 20;

constexpr uint64_t kValid = 2;
constexpr uint64_t kDirty = 4;

// A cartridge of 1 MiB.
constexpr size_t kRomSize = 0x100000;

uint32_t CauseCode(const Cpu& cpu)
{
    return static_cast<uint32_t>(cpu.Cop0(usf::n64::kCop0Cause) >> kCauseCodeShift) & kCauseCodeMask;
}

void ClearExl(Cpu& cpu)
{
    cpu.SetCop0(usf::n64::kCop0Status, cpu.Cop0(usf::n64::kCop0Status) & ~uint64_t{usf::n64::kStatusExl});
}

// An EntryLo value for the 4 KiB page at `physical`.
uint64_t EntryLo(uint32_t physical, uint64_t flags)
{
    return (uint64_t{physical} >> 12 << 6) | flags;
}

// Writes a TLB entry with TLBWI. A page mask of 0 gives two pages of 4 KiB.
void WriteTlbEntry(System& system, int index, uint64_t entry_hi, uint64_t entry_lo0, uint64_t entry_lo1,
                   uint64_t page_mask = 0)
{
    Cpu& cpu = system.GetCpu();
    cpu.SetCop0(usf::n64::kCop0Index, static_cast<uint64_t>(index));
    cpu.SetCop0(usf::n64::kCop0PageMask, page_mask);
    cpu.SetCop0(usf::n64::kCop0EntryHi, entry_hi);
    cpu.SetCop0(usf::n64::kCop0EntryLo0, entry_lo0);
    cpu.SetCop0(usf::n64::kCop0EntryLo1, entry_lo1);
    RunProgram(system, {kTlbWriteIndexed}, 1);
}

// A legacy player raises no exception for BREAK. Execution goes on at the next instruction.
void BreakRaisesNothing()
{
    for (const bool exact : {true, false})
    {
        usf::n64::Options options = SetOptions(false);
        options.exact_events = exact;
        System system(options);
        RunProgram(system, {kBreak, kSetT2}, 2);
        if (!exact)
        {
            // The BREAK has moved the clock past the end of the run.
            system.Run(system.TicksPerInstruction());
        }

        const Cpu& cpu = system.GetCpu();
        XSFLIB_CHECK(cpu.Pc() == kCodeAddress + 8);
        XSFLIB_CHECK(cpu.Gpr(10) == 1);
        XSFLIB_CHECK(CauseCode(cpu) != kCodeBreakpoint);
    }

    // The hardware raises the breakpoint exception (the VR4300's manual).
    System hardware;
    RunProgram(hardware, {kBreak}, 1);
    XSFLIB_CHECK(CauseCode(hardware.GetCpu()) == kCodeBreakpoint);
    XSFLIB_CHECK(hardware.GetCpu().Cop0(usf::n64::kCop0Epc) == kCodeAddress);
}

// With the legacy timing, BREAK moves Count to the time of the first event. The instructions since Count's last update,
// the BREAK among them, are still to add. The event is handled at the next check point.
void BreakMovesCountForward()
{
    constexpr uint64_t kEventTime = 0x1000;
    System system(SetOptions(false));
    const uint64_t ticks = system.TicksPerInstruction();
    system.Schedule(System::Event::kVi, kEventTime);

    RunProgram(system, {kNop, kNop, kBreak, kSetT2, kBranchOverOne, kNop, kNop}, 3);

    const Cpu& cpu = system.GetCpu();
    XSFLIB_CHECK(cpu.Pc() == kCodeAddress + 12);
    XSFLIB_CHECK(cpu.Cop0(usf::n64::kCop0Count) == (kEventTime + 3 * ticks) / 2);
    XSFLIB_CHECK((system.ReadPhysical32(kMiIntr) & usf::n64::kMiInterruptVi) == 0);

    system.Run(ticks);
    XSFLIB_CHECK(cpu.Gpr(10) == 1);
    XSFLIB_CHECK((system.ReadPhysical32(kMiIntr) & usf::n64::kMiInterruptVi) == 0);

    system.Run(2 * ticks);
    XSFLIB_CHECK((system.ReadPhysical32(kMiIntr) & usf::n64::kMiInterruptVi) != 0);
}

// When the event is already overdue, Count moves back to its time. The legacy timing doesn't bring Count up to date
// first. Only the instructions since the last update are therefore added. Here, the read of MI_INTR is that update.
void BreakMovesCountBack()
{
    constexpr uint64_t kEventTime = 4;
    System system(SetOptions(false));
    const uint64_t ticks = system.TicksPerInstruction();
    system.Schedule(System::Event::kVi, kEventTime);

    // The clock moves back, so the run goes on for one more instruction than it would without the BREAK.
    RunProgram(system, {kLuiT0Mi, kLoadMiIntr, kNop, kBreak, kSetT2}, 4);

    XSFLIB_CHECK(system.GetCpu().Gpr(10) == 1);
    XSFLIB_CHECK(system.GetCpu().Cop0(usf::n64::kCop0Count) == (kEventTime + 3 * ticks) / 2);
}

// VI_CURRENT is the count units since the last field over 1,500. In the interlaced modes, its bit 0 gives the field.
void ViCurrent()
{
    constexpr uint32_t kViStatus = 0x04400000;
    constexpr uint32_t kViCurrent = 0x04400010;
    constexpr uint32_t kSerrate = 0x40;
    constexpr uint64_t kTicksPerLine = 2 * 1500;
    constexpr uint64_t kSliceInstructions = 100;

    System system(SetOptions(false));
    const uint64_t slice = kSliceInstructions * system.TicksPerInstruction();
    RunProgram(system, {kNop, kBranchBackTwo, kNop}, 1);

    // The first field starts at time 0.
    uint32_t last = 0;
    while (system.Fields() == 0)
    {
        last = system.ReadPhysical32(kViCurrent);
        XSFLIB_CHECK(last == system.Now() / kTicksPerLine);
        system.Run(slice);
    }
    XSFLIB_CHECK(last > 500);

    // The half-line falls back with the new field, and starts again from the time that the interrupt was handled.
    XSFLIB_CHECK(system.ReadPhysical32(kViCurrent) == 0);

    system.WritePhysical32(kViStatus, kSerrate);
    while (system.ReadPhysical32(kViCurrent) < 100)
    {
        system.Run(slice);
    }

    for (const uint32_t field : {0u, 1u, 0u})
    {
        XSFLIB_CHECK((system.ReadPhysical32(kViCurrent) & 1) == field);
        const uint64_t fields = system.Fields();
        while (system.Fields() == fields)
        {
            XSFLIB_CHECK((system.ReadPhysical32(kViCurrent) & 1) == field);
            system.Run(slice);
        }
    }
}

std::vector<uint8_t> StateWithOptions()
{
    System source(SetOptions(false));
    source.LoadRom(usf::testing::TestRom(kRomSize));

    return source.SaveState();
}

// A state with IP2 in Cause, an enabled interrupt line and the AI's bit in MI_INTR, with that bit allowed by the mask.
std::vector<uint8_t> StaleInterruptState()
{
    System source(SetOptions(false));
    source.LoadRom(usf::testing::TestRom(kRomSize));
    source.WritePhysical32(kMiMask, 0x20); // Sets the AI's mask bit.
    source.RaiseMi(usf::n64::kMiInterruptAi);
    Cpu& cpu = source.GetCpu();
    cpu.SetCop0(usf::n64::kCop0Status, usf::n64::kStatusIe | kStatusLine2);
    cpu.SetCop0(usf::n64::kCop0Cause, usf::n64::kCauseIp2);

    return source.SaveState();
}

// A state's IP2 takes no interrupt. An AI bit in a state's MI_INTR never raises one alone, and stays readable until the
// first re-check. That re-check recomputes IP2 from MI_INTR and MI_INTR_MASK.
void StaleInterruptLines()
{
    const std::vector<uint8_t> state = StaleInterruptState();
    System system(SetOptions(false));
    system.LoadRom(usf::testing::TestRom(kRomSize));
    system.LoadState(state);
    const Cpu& cpu = system.GetCpu();

    RunProgram(system, {kBranchOverOne, kNop, kSetT2}, 3);

    XSFLIB_CHECK(cpu.Pc() == kCodeAddress + 12);
    XSFLIB_CHECK(cpu.Gpr(10) == 1);
    XSFLIB_CHECK((cpu.Cop0(usf::n64::kCop0Cause) & usf::n64::kCauseIp2) != 0);
    XSFLIB_CHECK((system.ReadPhysical32(kMiIntr) & usf::n64::kMiInterruptAi) != 0);

    system.RecheckInterrupts();
    XSFLIB_CHECK((cpu.Cop0(usf::n64::kCop0Cause) & usf::n64::kCauseIp2) == 0);
    XSFLIB_CHECK((system.ReadPhysical32(kMiIntr) & usf::n64::kMiInterruptAi) == 0);
}

// Each re-check copies the latch to MI_INTR's AI bit. In the legacy timing, a write of AI_STATUS re-checks the
// interrupt lines before it clears the latch. The bit therefore stays until the next re-check.
void AiLatch()
{
    System system(SetOptions(false));
    system.LoadRom(usf::testing::TestRom(kRomSize));
    const auto ai_bit = [&system]
    {
        return (system.ReadPhysical32(kMiIntr) & usf::n64::kMiInterruptAi) != 0;
    };

    XSFLIB_CHECK(!ai_bit());
    system.RaiseMi(usf::n64::kMiInterruptAi);
    XSFLIB_CHECK(ai_bit());
    system.RecheckInterrupts();
    XSFLIB_CHECK(ai_bit());

    system.WritePhysical32(kAiStatus, 0);
    XSFLIB_CHECK(ai_bit());
    system.RecheckInterrupts();
    XSFLIB_CHECK(!ai_bit());
}

// A store to a page with no writable mapping raises nothing, and the store is dropped.
void TlbStores()
{
    constexpr uint32_t kReadOnly = 0x10000;
    constexpr uint32_t kWritable = 0x11000;
    constexpr uint32_t kValue = 0x11223344;
    System system(SetOptions(false));
    WriteTlbEntry(system, 0, 0x00400000, EntryLo(kReadOnly, kValid), EntryLo(kWritable, kValid | kDirty));
    WriteTlbEntry(system, 1, 0x00600000, EntryLo(0x12000, kDirty), 0);
    Cpu& cpu = system.GetCpu();
    const std::span<uint8_t> ram = system.Rdram();

    const auto store_at = [&system, &cpu](uint64_t address)
    {
        cpu.SetGpr(8, address);
        cpu.SetGpr(9, kValue);
        cpu.SetGpr(10, 0);
        const uint32_t cause_before = static_cast<uint32_t>(cpu.Cop0(usf::n64::kCop0Cause));
        RunProgram(system, {kStoreT1, kSetT2}, 2);

        XSFLIB_CHECK(cpu.Pc() == kCodeAddress + 8);
        XSFLIB_CHECK(cpu.Gpr(10) == 1);
        XSFLIB_CHECK(static_cast<uint32_t>(cpu.Cop0(usf::n64::kCop0Cause)) == cause_before);
    };

    store_at(0x00400000); // Read-only.
    XSFLIB_CHECK(xsflib::LoadBe32(ram.data() + kReadOnly) == 0);
    store_at(0x00600000); // V clear, D set.
    XSFLIB_CHECK(xsflib::LoadBe32(ram.data() + 0x12000) == 0);
    store_at(0x00800000); // Unmapped.
    store_at(0x00401000); // Writable.
    XSFLIB_CHECK(xsflib::LoadBe32(ram.data() + kWritable) == kValue);
}

// What the exception of a fetch or load with no mapping sets, with EXL clear.
void TlbFaultFields()
{
    constexpr uint64_t kAddress = 0x00801234;
    System system(SetOptions(false));
    Cpu& cpu = system.GetCpu();
    WriteTlbEntry(system, 0, 0x00400000, EntryLo(0x10000, kValid), 0);

    // An ASID in EntryHi, and IP bits in Cause.
    cpu.SetCop0(usf::n64::kCop0EntryHi, 0x25);
    cpu.SetCop0(usf::n64::kCop0Cause, usf::n64::kCauseIp2 | 0x300);
    cpu.SetGpr(8, kAddress);
    RunProgram(system, {kLoadT3}, 1);

    XSFLIB_CHECK(cpu.Cop0(usf::n64::kCop0Cause) == kCodeTlbLoad << kCauseCodeShift);
    XSFLIB_CHECK(cpu.Cop0(usf::n64::kCop0Epc) == kCodeAddress);
    XSFLIB_CHECK((cpu.Cop0(usf::n64::kCop0Status) & usf::n64::kStatusExl) != 0);
    XSFLIB_CHECK(cpu.Cop0(usf::n64::kCop0BadVAddr) == kAddress);
    XSFLIB_CHECK(((cpu.Cop0(usf::n64::kCop0Context) >> 4) & 0x7FFFF) == kAddress >> 13);
    XSFLIB_CHECK(cpu.Cop0(usf::n64::kCop0EntryHi) == (kAddress & ~uint64_t{0x1FFF}));
    XSFLIB_CHECK(cpu.Pc() == kRefillVector);

    // A load in a delay slot sets BD, and EPC holds the branch.
    ClearExl(cpu);
    cpu.SetGpr(8, kAddress);
    RunProgram(system, {kBranchOverOne, kLoadT3}, 2);
    XSFLIB_CHECK(cpu.Cop0(usf::n64::kCop0Cause) == (usf::n64::kCauseBd | kCodeTlbLoad << kCauseCodeShift));
    XSFLIB_CHECK(cpu.Cop0(usf::n64::kCop0Epc) == kCodeAddress);

    // A fetch.
    ClearExl(cpu);
    cpu.SetPc(kAddress);
    system.Run(system.TicksPerInstruction());
    XSFLIB_CHECK(cpu.Cop0(usf::n64::kCop0Cause) == kCodeTlbLoad << kCauseCodeShift);
    XSFLIB_CHECK(cpu.Cop0(usf::n64::kCop0Epc) == kAddress);
    XSFLIB_CHECK(cpu.Cop0(usf::n64::kCop0BadVAddr) == kAddress);
    XSFLIB_CHECK(cpu.Pc() == kRefillVector);
}

// With EXL set, the vector is the general one and EXL stays set. The legacy players still subtract 4 from EPC for a
// load, and 4 more in a delay slot. A fetch leaves EPC alone.
void TlbFaultAtExceptionLevel()
{
    constexpr uint64_t kAddress = 0x00800000;
    constexpr uint64_t kEpc = 0xFFFFFFFF80005000ull;
    System system(SetOptions(false));
    Cpu& cpu = system.GetCpu();
    const auto set_level = [&cpu]
    {
        cpu.SetCop0(usf::n64::kCop0Status, cpu.Cop0(usf::n64::kCop0Status) | usf::n64::kStatusExl);
        cpu.SetCop0(usf::n64::kCop0Epc, kEpc);
        cpu.SetGpr(8, kAddress);
    };

    set_level();
    RunProgram(system, {kLoadT3}, 1);
    XSFLIB_CHECK(cpu.Pc() == kGeneralVector);
    XSFLIB_CHECK((cpu.Cop0(usf::n64::kCop0Status) & usf::n64::kStatusExl) != 0);
    XSFLIB_CHECK(cpu.Cop0(usf::n64::kCop0Epc) == kEpc - 4);

    set_level();
    RunProgram(system, {kBranchOverOne, kLoadT3}, 2);
    XSFLIB_CHECK(cpu.Pc() == kGeneralVector);
    XSFLIB_CHECK(cpu.Cop0(usf::n64::kCop0Epc) == kEpc - 8);
    XSFLIB_CHECK((cpu.Cop0(usf::n64::kCop0Cause) & usf::n64::kCauseBd) != 0);

    set_level();
    cpu.SetPc(kAddress);
    system.Run(system.TicksPerInstruction());
    XSFLIB_CHECK(cpu.Pc() == kGeneralVector);
    XSFLIB_CHECK(cpu.Cop0(usf::n64::kCop0Epc) == kEpc);
}

// With EXL clear, an address that an entry covers takes the general vector, and any other address takes the refill
// vector. The scan ignores the ASID, the global bit and the V bit, and takes in unused entries.
void TlbFaultVectors()
{
    System system(SetOptions(false));
    Cpu& cpu = system.GetCpu();
    const auto vector_of = [&system, &cpu](uint64_t address)
    {
        ClearExl(cpu);
        cpu.SetGpr(8, address);
        RunProgram(system, {kLoadT3}, 1);
        XSFLIB_CHECK(CauseCode(cpu) == kCodeTlbLoad);

        return cpu.Pc();
    };

    // A state's unused entry of zeros covers 0 to 0x1FFF, with the state's defined flag clear.
    std::vector<uint8_t> state = StateWithOptions();
    std::fill_n(state.begin() + usf::n64::kStateTlbOffset, kTlbEntries * kTlbEntrySize, uint8_t{0});
    system.LoadRom(usf::testing::TestRom(kRomSize));
    system.LoadState(state);
    XSFLIB_CHECK(vector_of(0x00001FFC) == kGeneralVector);
    XSFLIB_CHECK(vector_of(0x00002000) == kRefillVector);

    // Another ASID, the global bit clear, and both V bits clear, in the even half and the odd half.
    XSFLIB_CHECK(vector_of(0x00400000) == kRefillVector);
    WriteTlbEntry(system, 0, 0x00400005, 0, 0);
    cpu.SetCop0(usf::n64::kCop0EntryHi, 6);
    XSFLIB_CHECK(vector_of(0x00400000) == kGeneralVector);
    XSFLIB_CHECK(vector_of(0x00401000) == kGeneralVector);
    XSFLIB_CHECK(vector_of(0x00402000) == kRefillVector);

    // The page mask widens what an entry covers: 16 KiB a half.
    WriteTlbEntry(system, 1, 0x00800000, 0, 0, 0x6000);
    XSFLIB_CHECK(vector_of(0x00807000) == kGeneralVector);
    XSFLIB_CHECK(vector_of(0x00808000) == kRefillVector);
}

// A TLB write clears the pages of the entry that it replaces, without a look at the other entries. A page that another
// entry covers is lost until an entry that covers it is written. A fetch or load then takes the general vector, and a
// store is dropped.
void TlbLostPage()
{
    constexpr uint32_t kFirst = 0x10000;
    constexpr uint32_t kSecond = 0x12000;
    constexpr uint32_t kValue = 0x55667788;
    System system(SetOptions(false));
    Cpu& cpu = system.GetCpu();
    xsflib::StoreBe32(system.Rdram().data() + kSecond, 0xBBBBBBBB);

    WriteTlbEntry(system, 0, 0x00400000, EntryLo(kFirst, kValid | kDirty), 0);
    WriteTlbEntry(system, 1, 0x00400000, EntryLo(kSecond, kValid | kDirty), 0);
    cpu.SetGpr(8, 0x00400000);
    RunProgram(system, {kLoadT3}, 1);
    XSFLIB_CHECK(cpu.Gpr(11) == 0xFFFFFFFFBBBBBBBBull);

    // Entry 0 moves away. Entry 1 still covers the page, and the table no longer holds it.
    WriteTlbEntry(system, 0, 0x00600000, 0, 0);
    cpu.SetGpr(8, 0x00400000);
    cpu.SetGpr(9, kValue);
    cpu.SetGpr(10, 0);
    RunProgram(system, {kStoreT1, kSetT2}, 2);
    XSFLIB_CHECK(cpu.Gpr(10) == 1);
    XSFLIB_CHECK(xsflib::LoadBe32(system.Rdram().data() + kSecond) == 0xBBBBBBBB);
    RunProgram(system, {kLoadT3}, 1);
    XSFLIB_CHECK(CauseCode(cpu) == kCodeTlbLoad);
    XSFLIB_CHECK(cpu.Pc() == kGeneralVector);

    // Another write of entry 1 brings the page back.
    ClearExl(cpu);
    WriteTlbEntry(system, 1, 0x00400000, EntryLo(kSecond, kValid | kDirty), 0);
    cpu.SetGpr(8, 0x00400000);
    RunProgram(system, {kStoreT1}, 1);
    XSFLIB_CHECK(xsflib::LoadBe32(system.Rdram().data() + kSecond) == kValue);

    // A half whose V bit is clear clears nothing when it's replaced.
    WriteTlbEntry(system, 2, 0x00400000, EntryLo(kFirst, kDirty), 0);
    WriteTlbEntry(system, 2, 0x00600000, 0, 0);
    cpu.SetGpr(8, 0x00400000);
    RunProgram(system, {kLoadT3}, 1);
    XSFLIB_CHECK(cpu.Gpr(11) == 0x55667788);
}

// A state's program counter may lie in a mapped range. The fetch goes through the table.
void TlbFetch()
{
    constexpr uint32_t kPage = 0x10000;
    System system(SetOptions(false));
    WriteTlbEntry(system, 0, 0x70006000, EntryLo(kPage, kValid), 0);
    WriteProgram(system, kPage + 0x94, {kSetT2});
    Cpu& cpu = system.GetCpu();

    cpu.SetPc(0x70006094);
    system.Run(system.TicksPerInstruction());

    XSFLIB_CHECK(cpu.Gpr(10) == 1);
    XSFLIB_CHECK(cpu.Pc() == 0x70006098);
}

// Neither ASIDs nor the global bit count, and where entries overlap, the one written last wins.
void TlbOverlaps()
{
    constexpr uint32_t kFirst = 0x10000;
    constexpr uint32_t kSecond = 0x12000;
    System system(SetOptions(false));
    xsflib::StoreBe32(system.Rdram().data() + kFirst, 0xAAAAAAAA);
    xsflib::StoreBe32(system.Rdram().data() + kSecond, 0xBBBBBBBB);
    Cpu& cpu = system.GetCpu();

    const auto load = [&system, &cpu]
    {
        cpu.SetGpr(8, 0x00400000);
        RunProgram(system, {kLoadT3}, 1);

        return static_cast<uint32_t>(cpu.Gpr(11));
    };

    // The entry holds ASID 5 with the global bit clear, and the current ASID is 6.
    WriteTlbEntry(system, 0, 0x00400005, EntryLo(kFirst, kValid), 0);
    cpu.SetCop0(usf::n64::kCop0EntryHi, 6);
    XSFLIB_CHECK(load() == 0xAAAAAAAA);

    WriteTlbEntry(system, 1, 0x00400000, EntryLo(kSecond, kValid), 0);
    XSFLIB_CHECK(load() == 0xBBBBBBBB);

    WriteTlbEntry(system, 0, 0x00400000, EntryLo(kFirst, kValid), 0);
    XSFLIB_CHECK(load() == 0xAAAAAAAA);
}

// Each page has a read mapping and a write mapping. A half with V set and D clear sets only the read mapping, so stores
// still go to the other entry's page. Replacing that half clears only the read mapping.
void TlbSeparateMappings()
{
    constexpr uint32_t kWritable = 0x10000;
    constexpr uint32_t kReadOnly = 0x12000;
    constexpr uint32_t kValue = 0x55667788;
    System system(SetOptions(false));
    Cpu& cpu = system.GetCpu();
    xsflib::StoreBe32(system.Rdram().data() + kReadOnly, 0xBBBBBBBB);
    WriteTlbEntry(system, 0, 0x00400000, EntryLo(kWritable, kValid | kDirty), 0);
    WriteTlbEntry(system, 1, 0x00400000, EntryLo(kReadOnly, kValid), 0);

    cpu.SetGpr(8, 0x00400000);
    cpu.SetGpr(9, kValue);
    RunProgram(system, {kStoreT1, kLoadT3}, 2);

    XSFLIB_CHECK(xsflib::LoadBe32(system.Rdram().data() + kWritable) == kValue);
    XSFLIB_CHECK(xsflib::LoadBe32(system.Rdram().data() + kReadOnly) == 0xBBBBBBBB);
    XSFLIB_CHECK(cpu.Gpr(11) == 0xFFFFFFFFBBBBBBBBull);

    WriteTlbEntry(system, 1, 0x00600000, 0, 0);
    xsflib::StoreBe32(system.Rdram().data() + kWritable, 0);
    cpu.SetGpr(8, 0x00400000);
    RunProgram(system, {kStoreT1}, 1);
    XSFLIB_CHECK(xsflib::LoadBe32(system.Rdram().data() + kWritable) == kValue);
    RunProgram(system, {kLoadT3}, 1);
    XSFLIB_CHECK(CauseCode(cpu) == kCodeTlbLoad);
}

// A half that the player refuses to map changes no mapping. A physical address of 0x20000000 or more is one such half.
// It still covers its address.
void TlbRefusedHalf()
{
    constexpr uint32_t kPage = 0x10000;
    System system(SetOptions(false));
    Cpu& cpu = system.GetCpu();
    xsflib::StoreBe32(system.Rdram().data() + kPage, 0xAAAAAAAA);
    WriteTlbEntry(system, 0, 0x00400000, EntryLo(kPage, kValid), 0);
    WriteTlbEntry(system, 1, 0x00400000, EntryLo(0x20000000, kValid | kDirty), 0);

    cpu.SetGpr(8, 0x00400000);
    RunProgram(system, {kLoadT3}, 1);
    XSFLIB_CHECK(cpu.Gpr(11) == 0xFFFFFFFFAAAAAAAAull);

    WriteTlbEntry(system, 2, 0x00800000, EntryLo(0x20000000, kValid), 0);
    cpu.SetGpr(8, 0x00800000);
    RunProgram(system, {kLoadT3}, 1);
    XSFLIB_CHECK(CauseCode(cpu) == kCodeTlbLoad);
    XSFLIB_CHECK(cpu.Pc() == kGeneralVector);
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("legacy_timing",
                                    []
                                    {
                                        BreakRaisesNothing();
                                        BreakMovesCountForward();
                                        BreakMovesCountBack();
                                        ViCurrent();
                                        StaleInterruptLines();
                                        AiLatch();
                                        TlbStores();
                                        TlbFaultFields();
                                        TlbFaultAtExceptionLevel();
                                        TlbFaultVectors();
                                        TlbLostPage();
                                        TlbFetch();
                                        TlbOverlaps();
                                        TlbSeparateMappings();
                                        TlbRefusedHalf();
                                    });
}
