// SPDX-License-Identifier: MIT

// A small MIPS encoder for the drivers that the ripper writes into RDRAM and for the tests' programs. It encodes the
// few MIPS I instructions that they use, from the field layouts of the MIPS architecture's manuals.

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "n64/mips_registers.h"

namespace usf::rip::mips
{

// The general-purpose registers by their O32 names. The core uses the same names.
using Register = n64::mips::Register;
using enum n64::mips::Register;

inline constexpr uint32_t kNop = 0;

// Encodes one instruction. Immediates are the 16 bits that the instruction holds.
uint32_t Lui(Register rt, uint16_t immediate);
uint32_t Ori(Register rt, Register rs, uint16_t immediate);
uint32_t Addiu(Register rt, Register rs, uint16_t immediate);
uint32_t Andi(Register rt, Register rs, uint16_t immediate);
uint32_t Lbu(Register rt, Register base, uint16_t offset);
uint32_t Lw(Register rt, Register base, uint16_t offset);
uint32_t Sw(Register rt, Register base, uint16_t offset);
uint32_t Sll(Register rd, Register rt, uint32_t shift);
uint32_t Srl(Register rd, Register rt, uint32_t shift);
uint32_t Or(Register rd, Register rs, Register rt);
uint32_t And(Register rd, Register rs, Register rt);
uint32_t Addu(Register rd, Register rs, Register rt);
uint32_t Sltu(Register rd, Register rs, Register rt);
uint32_t Jr(Register rs);
uint32_t Jalr(Register rd, Register rs);

// The upper and lower halves of an address for a LUI and a following load, store or ADDIU, which sign-extend the lower
// half.
uint16_t High(uint32_t address);
uint16_t Low(uint32_t address);

// Instructions at increasing addresses from `base`, with branches and jumps to addresses in the same program.
class Assembler
{
public:
    explicit Assembler(uint32_t base);

    // The address of the next instruction.
    uint32_t Here() const;

    void Emit(uint32_t word);

    // Puts `value` in `rd`, sign-extended from 32 bits, with one instruction or two.
    void LoadImmediate(Register rd, uint32_t value);

    // Branches and jumps to `target`. Each is followed by its delay slot, which the caller emits.
    void Beq(Register rs, Register rt, uint32_t target);
    void Bne(Register rs, Register rt, uint32_t target);
    void J(uint32_t target);

    // A branch whose target comes later: Beq or Bne emits it with an offset of 0, and SetBranchTarget fills in the
    // offset once the target is known. `index` is the branch's position in Words().
    size_t Size() const;
    void SetBranchTarget(size_t index, uint32_t target);

    const std::vector<uint32_t>& Words() const;

private:
    uint16_t BranchOffset(uint32_t from, uint32_t target) const;

    uint32_t base_;
    std::vector<uint32_t> words_;
};

} // namespace usf::rip::mips
