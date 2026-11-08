// SPDX-License-Identifier: MIT

// The Nintendo 64's clocks, memory sizes and physical address map.

#pragma once

#include <cstdint>

namespace usf::n64
{

// The CPU's clock. Count ticks at half of it. The core counts time in ticks of this clock.
inline constexpr uint64_t kCpuClock = 93'750'000;

// The RCP's clock: the RSP, the DMA engines and the PI run at it. It's two thirds of the CPU's clock.
inline constexpr uint64_t kRcpClock = 62'500'000;

// The video clocks of the three television systems. The VI and the AI count in them.
inline constexpr uint64_t kVideoClockNtsc = 48'681'812;
inline constexpr uint64_t kVideoClockPal = 49'656'530;
inline constexpr uint64_t kVideoClockMpal = 48'628'316;

inline constexpr uint32_t kRdramSize4 = 0x400000;
inline constexpr uint32_t kRdramSize8 = 0x800000;
inline constexpr uint32_t kSpMemSize = 0x1000;
inline constexpr uint32_t kPifRamSize = 0x40;

// Physical addresses.
inline constexpr uint32_t kRdramRegisters = 0x03F00000;
inline constexpr uint32_t kSpDmem = 0x04000000;
inline constexpr uint32_t kSpImem = 0x04001000;
inline constexpr uint32_t kSpRegisters = 0x04040000;
inline constexpr uint32_t kSpPcRegister = 0x04080000;
inline constexpr uint32_t kDpCommandRegisters = 0x04100000;
inline constexpr uint32_t kDpSpanRegisters = 0x04200000;
inline constexpr uint32_t kMiRegisters = 0x04300000;
inline constexpr uint32_t kViRegisters = 0x04400000;
inline constexpr uint32_t kAiRegisters = 0x04500000;
inline constexpr uint32_t kPiRegisters = 0x04600000;
inline constexpr uint32_t kRiRegisters = 0x04700000;
inline constexpr uint32_t kSiRegisters = 0x04800000;
inline constexpr uint32_t kCartDomain2Address1 = 0x05000000;
inline constexpr uint32_t kCartDomain1Address1 = 0x06000000;
inline constexpr uint32_t kCartDomain2Address2 = 0x08000000;
inline constexpr uint32_t kCartRom = 0x10000000;
inline constexpr uint32_t kPifRom = 0x1FC00000;
inline constexpr uint32_t kPifRam = 0x1FC007C0;
inline constexpr uint32_t kCartDomain1Address3 = 0x1FD00000;

// Seta's Aleck64 arcade board adds memory and inputs above 0x80000000, where a console has nothing. Games reach them
// through the TLB.
inline constexpr uint32_t kBoardAddresses = 0x80000000;
inline constexpr uint32_t kAleck64Ram = 0xC0000000;
inline constexpr uint32_t kAleck64RamSize = 0x800000;
inline constexpr uint32_t kAleck64Inputs = 0xC0800000;
inline constexpr uint32_t kAleck64InputsSize = 0x1000;
inline constexpr uint32_t kAleck64Switches = 0xC0800000;
inline constexpr uint32_t kAleck64System = 0xC0800004;
inline constexpr uint32_t kAleck64Panel = 0xC0800008;

// The video chip of the Aleck64's E90 main board: a list of tiles, a palette and registers.
inline constexpr uint32_t kAleck64Tiles = 0xD0000000;
inline constexpr uint32_t kAleck64Palette = 0xD0010000;
inline constexpr uint32_t kAleck64VideoMemorySize = 0x1000;

// The debug port that emulators and development cartridges offer at the top of cartridge space. A program writes
// text into the buffer and its length into the length register.
inline constexpr uint32_t kIsViewer = 0x13FF0000;
inline constexpr uint32_t kIsViewerLength = 0x13FF0014;
inline constexpr uint32_t kIsViewerBuffer = 0x13FF0020;
inline constexpr uint32_t kIsViewerSize = 0x10000;

// MI_MODE's repeat mode, which RDRAM register writes during IPL3 use.
inline constexpr uint32_t kMiModeRepeat = 1u << 7;

// MI interrupt bits, in MI_INTR and MI_MASK.
inline constexpr uint32_t kMiInterruptSp = 1u << 0;
inline constexpr uint32_t kMiInterruptSi = 1u << 1;
inline constexpr uint32_t kMiInterruptAi = 1u << 2;
inline constexpr uint32_t kMiInterruptVi = 1u << 3;
inline constexpr uint32_t kMiInterruptPi = 1u << 4;
inline constexpr uint32_t kMiInterruptDp = 1u << 5;

// SP_STATUS bits.
inline constexpr uint32_t kSpStatusHalt = 1u << 0;
inline constexpr uint32_t kSpStatusBroke = 1u << 1;
inline constexpr uint32_t kSpStatusDmaBusy = 1u << 2;
inline constexpr uint32_t kSpStatusDmaFull = 1u << 3;
inline constexpr uint32_t kSpStatusIoFull = 1u << 4;
inline constexpr uint32_t kSpStatusSingleStep = 1u << 5;
inline constexpr uint32_t kSpStatusInterruptOnBreak = 1u << 6;
inline constexpr uint32_t kSpStatusSignal0 = 1u << 7;

// The television system, in libultra's numbering (osTvType).
enum class TvType : uint32_t
{
    kPal = 0,
    kNtsc = 1,
    kMpal = 2,
};

} // namespace usf::n64
