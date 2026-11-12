// SPDX-License-Identifier: MIT

// The fields of the VR4300's COP0 registers, its reset values and its pages. Only the CPU's source files include this
// header.

#pragma once

#include <cstdint>

#include "n64/bits.h"
#include "n64/constants.h"

namespace usf::n64
{

// A page of the TLB's smallest size, and of the tables of pages: 4 KiB.
inline constexpr int kPageBits = 12;
inline constexpr uint32_t kPageOffsetMask = 0xFFF;

// Index: the entry that TLBR and TLBWI use, and the bit that a probe sets when no entry matches.
inline constexpr uint32_t kIndexEntryMask = 0x3F;
inline constexpr uint32_t kIndexProbeFailure = 0x80000000;
inline constexpr uint32_t kIndexMask = kIndexProbeFailure | kIndexEntryMask;

// EntryLo0 and EntryLo1: G, V and D in the low bits, then C, then the PFN from bit 6. A TLB entry keeps the PFN, C, D
// and V, and keeps G only when both halves have it.
inline constexpr uint32_t kEntryLoMask = 0x3FFFFFFF;
inline constexpr uint32_t kEntryLoGlobal = 1u << 0;
inline constexpr uint32_t kEntryLoValid = 1u << 1;
inline constexpr uint32_t kEntryLoDirty = 1u << 2;
inline constexpr int kEntryLoPfnShift = 6;
inline constexpr uint32_t kEntryLoKeptBits = 0x03FFFFFE;

// PageMask. The mask takes effect in pairs of bits. kPageMaskPairs has the low bit of each pair. kMinPairMask spans the
// pair of 4 KiB pages that an entry of the smallest size covers.
inline constexpr uint32_t kPageMaskMask = 0x01FFE000;
inline constexpr uint32_t kPageMaskPairs = 0x01554000;
inline constexpr uint32_t kMinPairMask = 0x1FFF;

// EntryHi: R in the top two bits, VPN2, and the ASID.
inline constexpr uint64_t kEntryHiRegionMask = 0xC000000000000000ull;
inline constexpr int kRegionShift = 62;
inline constexpr uint64_t kVpn2Mask = 0x000000FFFFFFE000ull;
inline constexpr uint64_t kEntryHiAsidMask = 0xFF;
inline constexpr uint64_t kEntryHiMask = kEntryHiRegionMask | kVpn2Mask | kEntryHiAsidMask;

// Context and XContext: the PTE base that software writes, and the bits that an address exception sets. In XContext,
// those bits include R at kXContextRegionShift.
inline constexpr uint64_t kContextPteBaseMask = 0xFFFFFFFFFF800000ull;
inline constexpr uint64_t kContextBadVpn2Mask = 0x7FFFF0;
inline constexpr uint64_t kXContextPteBaseMask = 0xFFFFFFFE00000000ull;
inline constexpr uint64_t kXContextBadVpn2Mask = 0x1FFFFFFF0ull;
inline constexpr int kXContextRegionShift = 31;

inline constexpr uint32_t kStatusWriteMask = 0xFF57FFFF;
inline constexpr uint32_t kConfigWriteMask = 0x0F00800F;
inline constexpr uint32_t kWiredMask = 0x3F;
inline constexpr uint32_t kWatchLoMask = 0xFFFFFFFB;
inline constexpr uint32_t kWatchHiMask = 0xF;
inline constexpr uint32_t kParityErrorMask = 0xFF;

// Cause: ExcCode, CE, the software interrupts, and every bit that the register defines (BD, CE, IP and ExcCode).
inline constexpr int kCauseExceptionShift = 2;
inline constexpr uint32_t kCauseExceptionMask = 0x1Fu << kCauseExceptionShift;
inline constexpr int kCauseCoprocessorShift = 28;
inline constexpr uint32_t kCauseCoprocessorMask = 3u << kCauseCoprocessorShift;
inline constexpr uint32_t kCauseSoftwareMask = 0x300;
inline constexpr uint32_t kCauseDefinedMask = 0xB000FF7C;

// The eight interrupt lines: IP in Cause, and IM in Status at the same bits.
inline constexpr uint32_t kInterruptMask = 0xFF00;

// What PRId reads, and what Config holds at power-on.
inline constexpr uint32_t kPrId = 0x00000B22;
inline constexpr uint32_t kConfigReset = 0x7006E463;

// Where the CPU starts after a reset: the PIF ROM, through KSEG1.
inline constexpr uint64_t kResetVector = SignExtend32(kKseg1 + kPifRom);

// The bases of the exception vectors: KSEG0, or the PIF ROM while Status has BEV.
inline constexpr uint64_t kVectorBase = SignExtend32(kKseg0);
inline constexpr uint64_t kVectorBaseBoot = kResetVector + 0x200;

} // namespace usf::n64
