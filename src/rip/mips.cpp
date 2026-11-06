// SPDX-License-Identifier: MIT

#include "rip/mips.h"

#include <cstdint>
#include <stdexcept>

namespace usf::rip::mips
{

namespace
{

// The major opcodes and the SPECIAL group's function codes.
constexpr uint32_t kOpSpecial = 0x00;
constexpr uint32_t kOpJ = 0x02;
constexpr uint32_t kOpBeq = 0x04;
constexpr uint32_t kOpBne = 0x05;
constexpr uint32_t kOpAddiu = 0x09;
constexpr uint32_t kOpAndi = 0x0C;
constexpr uint32_t kOpOri = 0x0D;
constexpr uint32_t kOpLui = 0x0F;
constexpr uint32_t kOpLw = 0x23;
constexpr uint32_t kOpLbu = 0x24;
constexpr uint32_t kOpSh = 0x29;
constexpr uint32_t kOpSw = 0x2B;

constexpr uint32_t kFunctSll = 0x00;
constexpr uint32_t kFunctSrl = 0x02;
constexpr uint32_t kFunctJr = 0x08;
constexpr uint32_t kFunctJalr = 0x09;
constexpr uint32_t kFunctAddu = 0x21;
constexpr uint32_t kFunctAnd = 0x24;
constexpr uint32_t kFunctOr = 0x25;
constexpr uint32_t kFunctSltu = 0x2B;

uint32_t IType(uint32_t op, Register rs, Register rt, uint16_t immediate)
{
    return op << 26 | uint32_t{rs} << 21 | uint32_t{rt} << 16 | immediate;
}

uint32_t RType(Register rs, Register rt, Register rd, uint32_t shift, uint32_t funct)
{
    return kOpSpecial << 26 | uint32_t{rs} << 21 | uint32_t{rt} << 16 | uint32_t{rd} << 11 | (shift & 0x1F) << 6 |
           funct;
}

} // namespace

uint32_t Lui(Register rt, uint16_t immediate)
{
    return IType(kOpLui, kZero, rt, immediate);
}

uint32_t Ori(Register rt, Register rs, uint16_t immediate)
{
    return IType(kOpOri, rs, rt, immediate);
}

uint32_t Addiu(Register rt, Register rs, uint16_t immediate)
{
    return IType(kOpAddiu, rs, rt, immediate);
}

uint32_t Andi(Register rt, Register rs, uint16_t immediate)
{
    return IType(kOpAndi, rs, rt, immediate);
}

uint32_t Lbu(Register rt, Register base, uint16_t offset)
{
    return IType(kOpLbu, base, rt, offset);
}

uint32_t Lw(Register rt, Register base, uint16_t offset)
{
    return IType(kOpLw, base, rt, offset);
}

uint32_t Sw(Register rt, Register base, uint16_t offset)
{
    return IType(kOpSw, base, rt, offset);
}

uint32_t Sh(Register rt, Register base, uint16_t offset)
{
    return IType(kOpSh, base, rt, offset);
}

uint32_t Sll(Register rd, Register rt, uint32_t shift)
{
    return RType(kZero, rt, rd, shift, kFunctSll);
}

uint32_t Srl(Register rd, Register rt, uint32_t shift)
{
    return RType(kZero, rt, rd, shift, kFunctSrl);
}

uint32_t Or(Register rd, Register rs, Register rt)
{
    return RType(rs, rt, rd, 0, kFunctOr);
}

uint32_t And(Register rd, Register rs, Register rt)
{
    return RType(rs, rt, rd, 0, kFunctAnd);
}

uint32_t Addu(Register rd, Register rs, Register rt)
{
    return RType(rs, rt, rd, 0, kFunctAddu);
}

uint32_t Sltu(Register rd, Register rs, Register rt)
{
    return RType(rs, rt, rd, 0, kFunctSltu);
}

uint32_t Jr(Register rs)
{
    return RType(rs, kZero, kZero, 0, kFunctJr);
}

uint32_t Jalr(Register rd, Register rs)
{
    return RType(rs, kZero, rd, 0, kFunctJalr);
}

uint16_t High(uint32_t address)
{
    return static_cast<uint16_t>((address + 0x8000) >> 16);
}

uint16_t Low(uint32_t address)
{
    return static_cast<uint16_t>(address & 0xFFFF);
}

Assembler::Assembler(uint32_t base) : base_(base)
{
}

uint32_t Assembler::Here() const
{
    return base_ + static_cast<uint32_t>(4 * words_.size());
}

void Assembler::Emit(uint32_t word)
{
    words_.push_back(word);
}

void Assembler::LoadImmediate(Register rd, uint32_t value)
{
    const int32_t signed_value = static_cast<int32_t>(value);
    if (signed_value >= -0x8000 && signed_value < 0x8000)
    {
        Emit(Addiu(rd, kZero, static_cast<uint16_t>(value)));
        return;
    }

    Emit(Lui(rd, static_cast<uint16_t>(value >> 16)));
    if ((value & 0xFFFF) != 0)
    {
        Emit(Ori(rd, rd, static_cast<uint16_t>(value & 0xFFFF)));
    }
}

void Assembler::Beq(Register rs, Register rt, uint32_t target)
{
    Emit(IType(kOpBeq, rs, rt, BranchOffset(Here(), target)));
}

void Assembler::Bne(Register rs, Register rt, uint32_t target)
{
    Emit(IType(kOpBne, rs, rt, BranchOffset(Here(), target)));
}

void Assembler::J(uint32_t target)
{
    // A jump keeps the top four bits of the delay slot's address.
    if (((Here() + 4) & 0xF0000000) != (target & 0xF0000000) || (target & 3) != 0)
    {
        throw std::invalid_argument("a jump's target is out of its reach");
    }

    Emit(kOpJ << 26 | ((target >> 2) & 0x03FFFFFF));
}

size_t Assembler::Size() const
{
    return words_.size();
}

void Assembler::SetBranchTarget(size_t index, uint32_t target)
{
    const uint32_t from = base_ + static_cast<uint32_t>(4 * index);
    words_.at(index) = (words_[index] & 0xFFFF0000) | BranchOffset(from, target);
}

const std::vector<uint32_t>& Assembler::Words() const
{
    return words_;
}

uint16_t Assembler::BranchOffset(uint32_t from, uint32_t target) const
{
    // A branch's offset counts words from its delay slot.
    const int64_t offset = (static_cast<int64_t>(target) - static_cast<int64_t>(from + 4)) / 4;
    if (offset < -0x8000 || offset >= 0x8000 || (target & 3) != 0)
    {
        throw std::invalid_argument("a branch's target is out of its reach");
    }

    return static_cast<uint16_t>(offset);
}

} // namespace usf::rip::mips
