// SPDX-License-Identifier: MIT

// What the test programs built with the ripper's MIPS encoder share: the start of the AI, and the hand-off of a buffer.
// The tests that include this header link usf_rip.

#pragma once

#include <cstdint>

#include "rip/mips.h"

namespace usf::testing
{

// Starts the AI with DMA on and a bit rate of 15. `load_dac_rate` is the instruction that puts the DACRATE in t0. s2
// ends up with the base address of the AI registers.
inline void EmitAiSetup(rip::mips::Assembler& code, uint32_t load_dac_rate)
{
    namespace mips = rip::mips;

    code.Emit(mips::Lui(mips::kS2, 0xA450));
    code.Emit(load_dac_rate);
    code.Emit(mips::Sw(mips::kT0, mips::kS2, 0x10));
    code.Emit(mips::Addiu(mips::kT0, mips::kZero, 15));
    code.Emit(mips::Sw(mips::kT0, mips::kS2, 0x14));
    code.Emit(mips::Addiu(mips::kT0, mips::kZero, 1));
    code.Emit(mips::Sw(mips::kT0, mips::kS2, 0x8));
}

// Hands the AI a buffer of `bytes` bytes at the physical address `buffer_high` << 16, through t0. EmitAiSetup leaves
// the AI's base in s2.
inline void EmitAiBuffer(rip::mips::Assembler& code, uint16_t buffer_high, uint16_t bytes)
{
    namespace mips = rip::mips;

    code.Emit(mips::Lui(mips::kT0, buffer_high));
    code.Emit(mips::Sw(mips::kT0, mips::kS2, 0x0));
    code.Emit(mips::Addiu(mips::kT0, mips::kZero, bytes));
    code.Emit(mips::Sw(mips::kT0, mips::kS2, 0x4));
}

// Waits while AI_STATUS has any of its bits from `shift` to 31 set: 31 waits while the AI is full, and 30 while it's
// busy too.
inline void EmitAiWait(rip::mips::Assembler& code, uint32_t shift)
{
    namespace mips = rip::mips;

    const uint32_t wait = code.Here();
    code.Emit(mips::Lw(mips::kT0, mips::kS2, 0xC));
    code.Emit(mips::Srl(mips::kT0, mips::kT0, shift));
    code.Bne(mips::kT0, mips::kZero, wait);
    code.Emit(mips::kNop);
}

} // namespace usf::testing
