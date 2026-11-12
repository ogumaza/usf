// SPDX-License-Identifier: MIT

#include "n64/rsp.h"

#include <algorithm>

#include "xsflib/bytes.h"

#include "n64/mips_registers.h"
#include "n64/system.h"

namespace usf::n64
{
namespace
{

// SP_STATUS's write bits that have no partner: clear broke, and the clear and set bits of the SP interrupt.
constexpr uint32_t kSpWriteClearBroke = 1u << 2;
constexpr uint32_t kSpWriteClearInterrupt = 1u << 3;
constexpr uint32_t kSpWriteSetInterrupt = 1u << 4;

int32_t SignExtend16(uint32_t value)
{
    return static_cast<int16_t>(static_cast<uint16_t>(value));
}

// The 7-bit signed offset of a vector load or store.
int32_t VectorOffset(uint32_t instruction)
{
    const int32_t offset = static_cast<int32_t>(instruction & 0x7F);
    return offset >= 0x40 ? offset - 0x80 : offset;
}

// The fields of a vector load or store, and the value of its base register.
struct VectorAccess
{
    VectorAccess(uint32_t instruction, const std::array<uint32_t, 32>& gpr)
        : vt(static_cast<int>((instruction >> 16) & 31)),
          kind((instruction >> 11) & 31),
          e(static_cast<int>((instruction >> 7) & 15)),
          base(gpr[(instruction >> 21) & 31]),
          offset(VectorOffset(instruction))
    {
    }

    // The address of an access whose offset counts in units of `scale` bytes.
    uint32_t Address(int scale) const
    {
        return base + static_cast<uint32_t>(offset * scale);
    }

    int vt;
    uint32_t kind;
    int e;
    uint32_t base;
    int32_t offset;
};

} // namespace

Rsp::Rsp(System& system) : system_(system)
{
}

void Rsp::Reset()
{
    dmem_ = {};
    imem_ = {};
    gpr_ = {};
    pc_ = 0;
    next_pc_ = 4;
    status_ = kSpStatusHalt;
    semaphore_ = 0;
    ibist_ = 0;
    current_ = {};
    current_.length = kDmaLengthMask;
    pending_ = {};
    busy_ = false;
    full_ = false;
    vu_.Reset();
}

bool Rsp::Halted() const
{
    return (status_ & kSpStatusHalt) != 0;
}

Rsp::SpMemory& Rsp::Dmem()
{
    return dmem_;
}

Rsp::SpMemory& Rsp::Imem()
{
    return imem_;
}

const Rsp::SpMemory& Rsp::Dmem() const
{
    return dmem_;
}

const Rsp::SpMemory& Rsp::Imem() const
{
    return imem_;
}

uint32_t Rsp::Gpr(int index) const
{
    return gpr_[index];
}

uint32_t Rsp::Status() const
{
    uint32_t status = status_ & ~(kSpStatusDmaBusy | kSpStatusDmaFull);
    if (busy_)
    {
        status |= kSpStatusDmaBusy;
    }
    if (full_)
    {
        status |= kSpStatusDmaFull;
    }

    return status;
}

void Rsp::SetStatusBits(uint32_t bits)
{
    status_ |= bits;
}

bool Rsp::DmaBusy() const
{
    return busy_;
}

bool Rsp::DmaFull() const
{
    return full_;
}

uint64_t Rsp::Run(uint64_t cycles)
{
    uint64_t run = 0;
    while (run < cycles && !Halted())
    {
        Step();
        run++;
        if ((status_ & kSpStatusSingleStep) != 0)
        {
            status_ |= kSpStatusHalt;
        }
    }

    return run;
}

uint8_t Rsp::Read8(uint32_t address) const
{
    return dmem_[address & kAddressMask];
}

void Rsp::Write8(uint32_t address, uint8_t value)
{
    dmem_[address & kAddressMask] = value;
}

uint32_t Rsp::Read16(uint32_t address) const
{
    return (static_cast<uint32_t>(Read8(address)) << 8) | Read8(address + 1);
}

void Rsp::Write16(uint32_t address, uint32_t value)
{
    Write8(address, static_cast<uint8_t>(value >> 8));
    Write8(address + 1, static_cast<uint8_t>(value));
}

uint32_t Rsp::Read32(uint32_t address) const
{
    const uint32_t start = address & kAddressMask;
    if (start <= kSpMemSize - 4)
    {
        return xsflib::LoadBe32(dmem_.data() + start);
    }

    return (static_cast<uint32_t>(Read8(address)) << 24) | (static_cast<uint32_t>(Read8(address + 1)) << 16) |
           (static_cast<uint32_t>(Read8(address + 2)) << 8) | Read8(address + 3);
}

void Rsp::Write32(uint32_t address, uint32_t value)
{
    const uint32_t start = address & kAddressMask;
    if (start <= kSpMemSize - 4)
    {
        xsflib::StoreBe32(dmem_.data() + start, value);
        return;
    }

    Write8(address, static_cast<uint8_t>(value >> 24));
    Write8(address + 1, static_cast<uint8_t>(value >> 16));
    Write8(address + 2, static_cast<uint8_t>(value >> 8));
    Write8(address + 3, static_cast<uint8_t>(value));
}

void Rsp::LoadBytes(int vt, int first, int end, uint32_t address)
{
    // Whole lanes that don't wrap go together.
    const uint32_t start = address & kAddressMask;
    if (((first | end) & 1) == 0 && first < end && end <= 16 &&
        start + static_cast<uint32_t>(end - first) <= kSpMemSize)
    {
        const uint8_t* bytes = dmem_.data() + start;
        for (int lane = first / 2; lane < end / 2; lane++)
        {
            vu_.SetLane(vt, lane, xsflib::LoadBe16(bytes));
            bytes += 2;
        }

        return;
    }

    for (int index = first; index < end; index++)
    {
        vu_.SetByte(vt, index & 15, Read8(address++));
    }
}

void Rsp::StoreBytes(int vt, int first, int end, uint32_t address)
{
    // Whole lanes that don't wrap go together.
    const uint32_t start = address & kAddressMask;
    if (((first | end) & 1) == 0 && first < end && end <= 16 &&
        start + static_cast<uint32_t>(end - first) <= kSpMemSize)
    {
        uint8_t* bytes = dmem_.data() + start;
        for (int lane = first / 2; lane < end / 2; lane++)
        {
            xsflib::StoreBe16(bytes, vu_.Lane(vt, lane));
            bytes += 2;
        }

        return;
    }

    for (int index = first; index < end; index++)
    {
        Write8(address++, vu_.Byte(vt, index & 15));
    }
}

void Rsp::Step()
{
    const uint32_t pc = pc_;
    const uint32_t word = xsflib::LoadBe32(imem_.data() + pc);
    Decoded& decoded = decoded_[pc >> 2];
    if (decoded.word != word)
    {
        decoded = Decode(word);
    }

    pc_ = next_pc_;
    next_pc_ = (next_pc_ + 4) & kWordAddressMask;
    Execute(decoded);
    gpr_[0] = 0;
}

uint32_t Rsp::Link() const
{
    return (pc_ + 4) & kWordAddressMask;
}

void Rsp::Branch(bool taken, uint32_t target)
{
    if (taken)
    {
        next_pc_ = target & kWordAddressMask;
    }
}

Rsp::Decoded Rsp::Decode(uint32_t instruction)
{
    Decoded decoded;
    decoded.word = instruction;
    decoded.op = Op::kNone;
    decoded.rs = static_cast<uint8_t>((instruction >> 21) & 31);
    decoded.rt = static_cast<uint8_t>((instruction >> 16) & 31);
    decoded.rd = static_cast<uint8_t>((instruction >> 11) & 31);
    decoded.sa = static_cast<uint8_t>((instruction >> 6) & 31);
    const uint32_t zero_extended = instruction & 0xFFFF;
    const uint32_t sign_extended = static_cast<uint32_t>(SignExtend16(zero_extended));
    decoded.immediate = sign_extended;

    const auto set = [&decoded](Op op)
    {
        decoded.op = op;
    };

    const auto set_zero_extended = [&decoded, zero_extended](Op op)
    {
        decoded.op = op;
        decoded.immediate = zero_extended;
    };

    const auto set_branch = [&decoded, sign_extended](Op op)
    {
        decoded.op = op;
        decoded.immediate = sign_extended << 2;
    };

    const auto set_jump = [&decoded, instruction](Op op)
    {
        decoded.op = op;
        decoded.immediate = (instruction << 2) & kWordAddressMask;
    };

    switch (instruction >> 26)
    {
    case 0x00:
        set(SpecialOp(instruction & 0x3F));
        break;

    case 0x01:
        {
            const Op op = RegimmOp(decoded.rt);
            if (op != Op::kNone)
            {
                set_branch(op);
            }
            break;
        }

    case 0x02:
        set_jump(Op::kJ);
        break;

    case 0x03:
        set_jump(Op::kJal);
        break;

    case 0x04:
        set_branch(Op::kBeq);
        break;

    case 0x05:
        set_branch(Op::kBne);
        break;

    case 0x06:
        set_branch(Op::kBlez);
        break;

    case 0x07:
        set_branch(Op::kBgtz);
        break;

    case 0x08:
    case 0x09:
        set(Op::kAddi);
        break;

    case 0x0A:
        set(Op::kSlti);
        break;

    case 0x0B:
        set(Op::kSltiu);
        break;

    case 0x0C:
        set_zero_extended(Op::kAndi);
        break;

    case 0x0D:
        set_zero_extended(Op::kOri);
        break;

    case 0x0E:
        set_zero_extended(Op::kXori);
        break;

    case 0x0F:
        set_zero_extended(Op::kLui);
        break;

    case 0x10:
        set(Op::kCop0);
        break;

    case 0x12:
        if ((instruction & (1u << 25)) != 0)
        {
            set(Op::kVector);
            decoded.vector = VectorUnit::Decode(instruction);
            decoded.rs = static_cast<uint8_t>((instruction >> 21) & 15);
            break;
        }

        // The COP2 moves name a byte of the register in bits 7 to 10.
        decoded.sa = static_cast<uint8_t>((instruction >> 7) & 15);
        set(Cop2MoveOp(decoded.rs));
        break;

    case 0x20:
        set(Op::kLb);
        break;

    case 0x21:
        set(Op::kLh);
        break;

    case 0x23:
    case 0x27:
        set(Op::kLw);
        break;

    case 0x24:
        set(Op::kLbu);
        break;

    case 0x25:
        set(Op::kLhu);
        break;

    case 0x28:
        set(Op::kSb);
        break;

    case 0x29:
        set(Op::kSh);
        break;

    case 0x2B:
        set(Op::kSw);
        break;

    case 0x32:
        set(Op::kLoadVector);
        break;

    case 0x3A:
        set(Op::kStoreVector);
        break;

    default:
        break;
    }

    return decoded;
}

Rsp::Op Rsp::SpecialOp(uint32_t funct)
{
    switch (funct)
    {
    case 0x00:
        return Op::kSll;
    case 0x02:
        return Op::kSrl;
    case 0x03:
        return Op::kSra;
    case 0x04:
        return Op::kSllv;
    case 0x06:
        return Op::kSrlv;
    case 0x07:
        return Op::kSrav;
    case 0x08:
        return Op::kJr;
    case 0x09:
        return Op::kJalr;
    case 0x0D:
        return Op::kBreak;
    case 0x20:
    case 0x21:
        return Op::kAdd;
    case 0x22:
    case 0x23:
        return Op::kSub;
    case 0x24:
        return Op::kAnd;
    case 0x25:
        return Op::kOr;
    case 0x26:
        return Op::kXor;
    case 0x27:
        return Op::kNor;
    case 0x2A:
        return Op::kSlt;
    case 0x2B:
        return Op::kSltu;
    default:
        return Op::kNone;
    }
}

Rsp::Op Rsp::RegimmOp(uint32_t rt)
{
    switch (rt)
    {
    case 0x00:
        return Op::kBltz;
    case 0x01:
        return Op::kBgez;
    case 0x10:
        return Op::kBltzal;
    case 0x11:
        return Op::kBgezal;
    default:
        return Op::kNone;
    }
}

Rsp::Op Rsp::Cop2MoveOp(uint32_t rs)
{
    switch (rs)
    {
    case 0x00:
        return Op::kMfc2;
    case 0x02:
        return Op::kCfc2;
    case 0x04:
        return Op::kMtc2;
    case 0x06:
        return Op::kCtc2;
    default:
        return Op::kNone;
    }
}

void Rsp::Execute(const Decoded& instruction)
{
    const uint32_t rs = instruction.rs;
    const uint32_t rt = instruction.rt;
    const uint32_t rd = instruction.rd;
    const uint32_t immediate = instruction.immediate;

    // pc_ already holds the address of the delay slot.
    switch (instruction.op)
    {
    case Op::kSll:
        gpr_[rd] = gpr_[rt] << instruction.sa;
        break;

    case Op::kSrl:
        gpr_[rd] = gpr_[rt] >> instruction.sa;
        break;

    case Op::kSra:
        gpr_[rd] = static_cast<uint32_t>(static_cast<int32_t>(gpr_[rt]) >> instruction.sa);
        break;

    case Op::kSllv:
        gpr_[rd] = gpr_[rt] << (gpr_[rs] & 31);
        break;

    case Op::kSrlv:
        gpr_[rd] = gpr_[rt] >> (gpr_[rs] & 31);
        break;

    case Op::kSrav:
        gpr_[rd] = static_cast<uint32_t>(static_cast<int32_t>(gpr_[rt]) >> (gpr_[rs] & 31));
        break;

    case Op::kJr:
        Branch(true, gpr_[rs]);
        break;

    case Op::kJalr:
        Branch(true, gpr_[rs]);
        gpr_[rd] = Link();
        break;

    case Op::kBreak:
        // BREAK halts the RSP and raises its interrupt if SP_STATUS asks for one.
        status_ |= kSpStatusHalt | kSpStatusBroke;
        system_.OnRspBroke();
        break;

    case Op::kAdd:
        gpr_[rd] = gpr_[rs] + gpr_[rt];
        break;

    case Op::kSub:
        gpr_[rd] = gpr_[rs] - gpr_[rt];
        break;

    case Op::kAnd:
        gpr_[rd] = gpr_[rs] & gpr_[rt];
        break;

    case Op::kOr:
        gpr_[rd] = gpr_[rs] | gpr_[rt];
        break;

    case Op::kXor:
        gpr_[rd] = gpr_[rs] ^ gpr_[rt];
        break;

    case Op::kNor:
        gpr_[rd] = ~(gpr_[rs] | gpr_[rt]);
        break;

    case Op::kSlt:
        gpr_[rd] = static_cast<int32_t>(gpr_[rs]) < static_cast<int32_t>(gpr_[rt]) ? 1 : 0;
        break;

    case Op::kSltu:
        gpr_[rd] = gpr_[rs] < gpr_[rt] ? 1 : 0;
        break;

    case Op::kBltz:
        Branch(static_cast<int32_t>(gpr_[rs]) < 0, pc_ + immediate);
        break;

    case Op::kBgez:
        Branch(static_cast<int32_t>(gpr_[rs]) >= 0, pc_ + immediate);
        break;

    case Op::kBltzal:
        Branch(static_cast<int32_t>(gpr_[rs]) < 0, pc_ + immediate);
        gpr_[mips::kRa] = Link();
        break;

    case Op::kBgezal:
        Branch(static_cast<int32_t>(gpr_[rs]) >= 0, pc_ + immediate);
        gpr_[mips::kRa] = Link();
        break;

    case Op::kJ:
        Branch(true, immediate);
        break;

    case Op::kJal:
        Branch(true, immediate);
        gpr_[mips::kRa] = Link();
        break;

    case Op::kBeq:
        Branch(gpr_[rs] == gpr_[rt], pc_ + immediate);
        break;

    case Op::kBne:
        Branch(gpr_[rs] != gpr_[rt], pc_ + immediate);
        break;

    case Op::kBlez:
        Branch(static_cast<int32_t>(gpr_[rs]) <= 0, pc_ + immediate);
        break;

    case Op::kBgtz:
        Branch(static_cast<int32_t>(gpr_[rs]) > 0, pc_ + immediate);
        break;

    case Op::kAddi:
        gpr_[rt] = gpr_[rs] + immediate;
        break;

    case Op::kSlti:
        gpr_[rt] = static_cast<int32_t>(gpr_[rs]) < static_cast<int32_t>(immediate) ? 1 : 0;
        break;

    case Op::kSltiu:
        gpr_[rt] = gpr_[rs] < immediate ? 1 : 0;
        break;

    case Op::kAndi:
        gpr_[rt] = gpr_[rs] & immediate;
        break;

    case Op::kOri:
        gpr_[rt] = gpr_[rs] | immediate;
        break;

    case Op::kXori:
        gpr_[rt] = gpr_[rs] ^ immediate;
        break;

    case Op::kLui:
        gpr_[rt] = immediate << 16;
        break;

    case Op::kCop0:
        ExecuteCop0(instruction.word);
        break;

    case Op::kMfc2:
        {
            // MFC2: two bytes from byte `element`, wrapping within the register.
            const int element = instruction.sa;
            const uint32_t high = vu_.Byte(static_cast<int>(rd), element);
            const uint32_t low = vu_.Byte(static_cast<int>(rd), (element + 1) & 15);
            gpr_[rt] = static_cast<uint32_t>(SignExtend16((high << 8) | low));
            break;
        }

    case Op::kCfc2:
        gpr_[rt] = vu_.ReadControl(static_cast<int>(rd));
        break;

    case Op::kMtc2:
        // MTC2: two bytes at byte `element`. At element 15 only the high byte fits.
        vu_.SetByte(static_cast<int>(rd), instruction.sa, static_cast<uint8_t>(gpr_[rt] >> 8));
        if (instruction.sa != 15)
        {
            vu_.SetByte(static_cast<int>(rd), instruction.sa + 1, static_cast<uint8_t>(gpr_[rt]));
        }
        break;

    case Op::kCtc2:
        vu_.WriteControl(static_cast<int>(rd), gpr_[rt]);
        break;

    case Op::kVector:
        // A vector instruction's vd, vs, vt and element are the sa, rd, rt and rs fields.
        instruction.vector(vu_, instruction.sa, static_cast<int>(rd), static_cast<int>(rt), static_cast<int>(rs));
        break;

    case Op::kLb:
        gpr_[rt] = static_cast<uint32_t>(static_cast<int8_t>(Read8(gpr_[rs] + immediate)));
        break;

    case Op::kLh:
        gpr_[rt] = static_cast<uint32_t>(SignExtend16(Read16(gpr_[rs] + immediate)));
        break;

    case Op::kLw:
        gpr_[rt] = Read32(gpr_[rs] + immediate);
        break;

    case Op::kLbu:
        gpr_[rt] = Read8(gpr_[rs] + immediate);
        break;

    case Op::kLhu:
        gpr_[rt] = Read16(gpr_[rs] + immediate);
        break;

    case Op::kSb:
        Write8(gpr_[rs] + immediate, static_cast<uint8_t>(gpr_[rt]));
        break;

    case Op::kSh:
        Write16(gpr_[rs] + immediate, gpr_[rt]);
        break;

    case Op::kSw:
        Write32(gpr_[rs] + immediate, gpr_[rt]);
        break;

    case Op::kLoadVector:
        LoadVector(instruction.word);
        break;

    case Op::kStoreVector:
        StoreVector(instruction.word);
        break;

    case Op::kNone:
        break;
    }
}

void Rsp::ExecuteCop0(uint32_t instruction)
{
    const uint32_t rt = (instruction >> 16) & 31;
    const uint32_t rd = (instruction >> 11) & 15;
    switch ((instruction >> 21) & 31)
    {
    case 0x00:
        gpr_[rt] = ReadCop0(rd);
        break;
    case 0x04:
        WriteCop0(rd, gpr_[rt]);
        break;
    default:
        break;
    }
}

uint32_t Rsp::ReadCop0(uint32_t index)
{
    if (index >= 8)
    {
        return system_.ReadDp(index - 8);
    }

    return ReadRegister(index);
}

void Rsp::WriteCop0(uint32_t index, uint32_t value)
{
    if (index >= 8)
    {
        system_.WriteDp(index - 8, value);
        return;
    }

    WriteRegister(index, value);
}

uint32_t Rsp::ReadRegister(uint32_t index)
{
    switch (index & 7)
    {
    case 0:
        return current_.mem_address & kDmaSpAddressMask;

    case 1:
        return current_.dram_address & kDmaRdramAddressMask;

    case 2:
    case 3:
        return current_.LengthRegister();

    case 4:
        return Status();

    case 5:
        return full_ ? 1 : 0;

    case 6:
        return busy_ ? 1 : 0;

    default:
        {
            // Reading the semaphore takes it.
            const uint32_t value = semaphore_;
            semaphore_ = 1;
            return value;
        }
    }
}

void Rsp::WriteRegister(uint32_t index, uint32_t value)
{
    switch (index & 7)
    {
    case 0:
        pending_.mem_address = value & kDmaSpAddressMask;
        break;
    case 1:
        pending_.dram_address = value & kDmaRdramAddressMask;
        break;
    case 2:
        QueueDma(value, false);
        break;
    case 3:
        QueueDma(value, true);
        break;
    case 4:
        WriteStatus(value);
        break;
    case 7:
        semaphore_ = 0;
        break;
    default:
        break;
    }
}

void Rsp::WriteStatus(uint32_t value)
{
    const uint32_t old_status = status_;

    // Halt, single step, interrupt on break and each signal have a clear bit and the set bit above it.
    const auto apply = [this, value](int clear_bit, int set_bit, uint32_t flag)
    {
        const bool clear = ((value >> clear_bit) & 1) != 0;
        const bool set = ((value >> set_bit) & 1) != 0;
        if (clear && !set)
        {
            status_ &= ~flag;
        }
        else if (set && !clear)
        {
            status_ |= flag;
        }
    };

    apply(0, 1, kSpStatusHalt);
    if ((value & kSpWriteClearBroke) != 0)
    {
        status_ &= ~kSpStatusBroke;
    }

    const bool clear_interrupt = (value & kSpWriteClearInterrupt) != 0;
    const bool set_interrupt = (value & kSpWriteSetInterrupt) != 0;
    if (clear_interrupt && !set_interrupt)
    {
        system_.LowerMi(kMiInterruptSp);
    }
    else if (set_interrupt && !clear_interrupt)
    {
        system_.RaiseMi(kMiInterruptSp);
    }

    apply(5, 6, kSpStatusSingleStep);
    apply(7, 8, kSpStatusInterruptOnBreak);
    for (int signal = 0; signal < 8; signal++)
    {
        apply(9 + 2 * signal, 10 + 2 * signal, kSpStatusSignal0 << signal);
    }

    system_.OnSpStatusWritten(old_status);
}

uint32_t Rsp::ReadPc() const
{
    return pc_ & kWordAddressMask;
}

void Rsp::WritePc(uint32_t value)
{
    pc_ = value & kWordAddressMask;
    next_pc_ = (pc_ + 4) & kWordAddressMask;
}

uint32_t Rsp::ReadIbist() const
{
    return ibist_;
}

void Rsp::WriteIbist(uint32_t value)
{
    ibist_ = value & 7;
}

uint32_t Rsp::Dma::LengthRegister() const
{
    return (length & kDmaLengthMask) | (count << kDmaCountShift) | (skip << kDmaSkipShift);
}

void Rsp::Dma::SetLengthRegister(uint32_t value)
{
    length = value & kDmaLengthMask;
    count = (value >> kDmaCountShift) & kDmaCountMask;
    skip = (value >> kDmaSkipShift) & kDmaLengthMask;
}

void Rsp::QueueDma(uint32_t length_register, bool to_rdram)
{
    pending_.SetLengthRegister(length_register);
    pending_.to_rdram = to_rdram;
    full_ = true;

    if (!busy_)
    {
        StartDma();
    }
}

uint64_t Rsp::DmaTicks(const Dma& dma) const
{
    // Three RCP cycles for each 8 bytes of each row. An RCP cycle is one and a half ticks.
    const uint64_t rcp_cycles = uint64_t{dma.count + 1} * ((dma.length + 8) / 8) * 3;
    return rcp_cycles * 3 / 2;
}

void Rsp::StartDma()
{
    current_ = pending_;
    busy_ = true;
    full_ = false;

    if (system_.GetOptions().timing == Timing::kLegacy)
    {
        FinishDma();
        return;
    }

    system_.Schedule(System::Event::kSpDma, system_.Now() + DmaTicks(current_));
}

void Rsp::FinishDma()
{
    if (!busy_)
    {
        return;
    }

    // Each row copies (length + 8) bytes in 8-byte units. SP memory addresses wrap within DMEM or IMEM, and the RDRAM
    // address skips `skip` bytes between rows. PeterLemon's SP DMA tests show no skip after the last row.
    SpMemory& memory = Memory(current_.mem_address);
    uint32_t mem = current_.mem_address & kDmaLengthMask;
    uint32_t dram = current_.dram_address & kDmaRdramAddressMask;
    for (uint32_t row = 0; row <= current_.count; row++)
    {
        for (uint32_t i = 0; i <= current_.length; i += 8)
        {
            std::array<uint8_t, 8> bytes;
            if (current_.to_rdram)
            {
                for (uint32_t b = 0; b < 8; b++)
                {
                    bytes[b] = memory[(mem + b) & kAddressMask];
                }
                system_.DmaWriteRdram(dram, bytes.data(), 8);
            }
            else
            {
                system_.DmaReadRdram(dram, bytes.data(), 8);
                for (uint32_t b = 0; b < 8; b++)
                {
                    memory[(mem + b) & kAddressMask] = bytes[b];
                }
            }

            mem = (mem + 8) & kDmaLengthMask;
            dram = (dram + 8) & kDmaRdramAddressMask;
        }

        if (row < current_.count)
        {
            dram = (dram + current_.skip) & kDmaRdramAddressMask;
        }
    }

    // The registers read back where the transfer ended, with a length of 0xFF8 and a count of 0.
    current_.mem_address = (current_.mem_address & kImemBit) | mem;
    current_.dram_address = dram;
    current_.length = kDmaLengthMask;
    current_.count = 0;
    busy_ = false;

    if (full_)
    {
        StartDma();
    }
}

SpRegisters Rsp::Registers() const
{
    SpRegisters registers;
    registers.mem_address = current_.mem_address & kDmaSpAddressMask;
    registers.dram_address = current_.dram_address & kDmaRdramAddressMask;
    registers.read_length = current_.LengthRegister();
    registers.write_length = registers.read_length;
    registers.status = Status();
    registers.dma_full = full_ ? 1 : 0;
    registers.dma_busy = busy_ ? 1 : 0;
    registers.semaphore = semaphore_;
    registers.pc = pc_ & kWordAddressMask;
    registers.ibist = ibist_;

    return registers;
}

void Rsp::SetRegisters(const SpRegisters& registers)
{
    current_ = {};
    current_.mem_address = registers.mem_address & kDmaSpAddressMask;
    current_.dram_address = registers.dram_address & kDmaRdramAddressMask;
    current_.SetLengthRegister(registers.read_length);
    pending_ = current_;
    status_ = registers.status & ~(kSpStatusDmaBusy | kSpStatusDmaFull | kSpStatusIoFull);
    busy_ = false;
    full_ = false;
    semaphore_ = registers.semaphore & 1;
    WritePc(registers.pc);
    ibist_ = registers.ibist & 7;
}

void Rsp::LoadVector(uint32_t instruction)
{
    const VectorAccess access(instruction, gpr_);
    const int vt = access.vt;
    const int e = access.e;
    switch (access.kind)
    {
    case 0x00:
        // LBV.
        vu_.SetByte(vt, e, Read8(access.Address(1)));
        break;

    case 0x01:
    case 0x02:
    case 0x03:
        {
            // LSV, LLV and LDV: 2, 4 or 8 bytes from byte e, stopping at the end of the register.
            const int size = 1 << access.kind;
            LoadBytes(vt, e, std::min(e + size, 16), access.Address(size));
            break;
        }

    case 0x04:
        {
            // LQV: up to the next 16-byte boundary of DMEM.
            const uint32_t address = access.Address(16);
            LoadBytes(vt, e, std::min(16 + e - static_cast<int>(address & 15), 16), address);
            break;
        }

    case 0x05:
        {
            // LRV: from the 16-byte boundary below the address up to it, into the end of the register.
            uint32_t address = access.Address(16);
            const int start = 16 - (static_cast<int>(address & 15) - e);
            LoadBytes(vt, start, 16, address & ~15u);
            break;
        }

    case 0x06:
    case 0x07:
        {
            // LPV and LUV: eight bytes into the high bits of the lanes, packed or unsigned.
            uint32_t address = access.Address(8);
            const int index = static_cast<int>(address & 7) - e;
            address &= ~7u;
            const int shift = access.kind == 0x06 ? 8 : 7;
            for (int lane = 0; lane < 8; lane++)
            {
                const uint32_t byte = Read8(address + static_cast<uint32_t>((index + lane) & 15));
                vu_.SetLane(vt, lane, static_cast<uint16_t>(byte << shift));
            }
            break;
        }

    case 0x08:
        {
            // LHV: every other byte.
            uint32_t address = access.Address(16);
            const int index = static_cast<int>(address & 7) - e;
            address &= ~7u;
            for (int lane = 0; lane < 8; lane++)
            {
                const uint32_t byte = Read8(address + static_cast<uint32_t>((index + lane * 2) & 15));
                vu_.SetLane(vt, lane, static_cast<uint16_t>(byte << 7));
            }
            break;
        }

    case 0x09:
        {
            // LFV: every fourth byte into four lanes, as eight bytes from byte e.
            uint32_t address = access.Address(16);
            const int index = static_cast<int>(address & 7) - e;
            address &= ~7u;
            std::array<uint16_t, 8> lanes;
            for (int i = 0; i < 4; i++)
            {
                lanes[i] = static_cast<uint16_t>(Read8(address + static_cast<uint32_t>((index + i * 4) & 15)) << 7);
                lanes[i + 4] =
                    static_cast<uint16_t>(Read8(address + static_cast<uint32_t>((index + i * 4 + 8) & 15)) << 7);
            }

            const int end = std::min(e + 8, 16);
            for (int byte = e; byte < end; byte++)
            {
                const uint16_t lane = lanes[byte / 2];
                vu_.SetByte(vt, byte, static_cast<uint8_t>((byte & 1) != 0 ? lane : lane >> 8));
            }
            break;
        }

    case 0x0B:
        {
            // LTV: a transpose. Each of eight registers gets one lane, rotating with the element.
            uint32_t address = access.Address(16);
            const uint32_t begin = address & ~7u;
            address = begin + ((static_cast<uint32_t>(e) + (address & 8)) & 15);
            const int vt_base = vt & ~7;
            int vt_offset = e >> 1;
            for (int i = 0; i < 8; i++)
            {
                vu_.SetByte(vt_base + vt_offset, i * 2, Read8(address++));
                if (address == begin + 16)
                {
                    address = begin;
                }

                vu_.SetByte(vt_base + vt_offset, i * 2 + 1, Read8(address++));
                if (address == begin + 16)
                {
                    address = begin;
                }

                vt_offset = (vt_offset + 1) & 7;
            }
            break;
        }

    default:
        // LWV and the reserved loads do nothing.
        break;
    }
}

void Rsp::StoreVector(uint32_t instruction)
{
    const VectorAccess access(instruction, gpr_);
    const int vt = access.vt;
    const int e = access.e;
    switch (access.kind)
    {
    case 0x00:
        // SBV.
        Write8(access.Address(1), vu_.Byte(vt, e));
        break;

    case 0x01:
    case 0x02:
    case 0x03:
        {
            // SSV, SLV and SDV wrap within the register.
            const int size = 1 << access.kind;
            StoreBytes(vt, e, e + size, access.Address(size));
            break;
        }

    case 0x04:
        {
            // SQV: up to the next 16-byte boundary.
            const uint32_t address = access.Address(16);
            StoreBytes(vt, e, e + (16 - static_cast<int>(address & 15)), address);
            break;
        }

    case 0x05:
        {
            // SRV: from the 16-byte boundary below the address up to it.
            uint32_t address = access.Address(16);
            const int end = e + static_cast<int>(address & 15);
            const int shift = 16 - static_cast<int>(address & 15);
            address &= ~15u;
            for (int index = e; index < end; index++)
            {
                Write8(address++, vu_.Byte(vt, (index + shift) & 15));
            }
            break;
        }

    case 0x06:
    case 0x07:
        {
            // SPV and SUV: the high byte of each lane, packed or unsigned. Past byte 8 of the register each swaps to
            // the other's form.
            uint32_t address = access.Address(8);
            const bool packed = access.kind == 0x06;
            for (int index = e; index < e + 8; index++)
            {
                const bool first_half = (index & 15) < 8;
                uint8_t value = 0;
                if (first_half == packed)
                {
                    value = vu_.Byte(vt, (index & 7) << 1);
                }
                else
                {
                    value = static_cast<uint8_t>(vu_.Lane(vt, index & 7) >> 7);
                }

                Write8(address++, value);
            }
            break;
        }

    case 0x08:
        {
            // SHV: every other byte, from bits 14 to 7 of each lane.
            uint32_t address = access.Address(16);
            const int index = static_cast<int>(address & 7);
            address &= ~7u;
            for (int i = 0; i < 8; i++)
            {
                const int byte = e + i * 2;
                const uint32_t value = (static_cast<uint32_t>(vu_.Byte(vt, byte & 15)) << 1) |
                                       (static_cast<uint32_t>(vu_.Byte(vt, (byte + 1) & 15)) >> 7);
                Write8(address + static_cast<uint32_t>((index + i * 2) & 15), static_cast<uint8_t>(value));
            }
            break;
        }

    case 0x09:
        {
            // SFV: every fourth byte, from four lanes that the element picks. Other elements store zeros.
            uint32_t address = access.Address(16);
            const uint32_t index = address & 7;
            address &= ~7u;
            std::array<int, 4> lanes = {-1, -1, -1, -1};
            switch (e)
            {
            case 0:
            case 15:
                lanes = {0, 1, 2, 3};
                break;
            case 1:
                lanes = {6, 7, 4, 5};
                break;
            case 4:
                lanes = {1, 2, 3, 0};
                break;
            case 5:
                lanes = {7, 4, 5, 6};
                break;
            case 8:
                lanes = {4, 5, 6, 7};
                break;
            case 11:
                lanes = {3, 0, 1, 2};
                break;
            case 12:
                lanes = {5, 6, 7, 4};
                break;
            default:
                break;
            }

            for (int i = 0; i < 4; i++)
            {
                const uint8_t value = lanes[i] < 0 ? 0 : static_cast<uint8_t>(vu_.Lane(vt, lanes[i]) >> 7);
                Write8(address + ((index + static_cast<uint32_t>(i) * 4) & 15), value);
            }
            break;
        }

    case 0x0A:
        {
            // SWV: the register rotated by the element, within the 8-byte-aligned block.
            uint32_t address = access.Address(16);
            uint32_t position = address & 7;
            address &= ~7u;
            for (int index = e; index < e + 16; index++)
            {
                Write8(address + (position++ & 15), vu_.Byte(vt, index & 15));
            }
            break;
        }

    case 0x0B:
        {
            // STV: a transpose. Each of eight registers gives one lane.
            uint32_t address = access.Address(16);
            const int start = vt & ~7;
            int byte = 16 - (e & ~1);
            uint32_t position = (address & 7) - static_cast<uint32_t>(e & ~1);
            address &= ~7u;
            for (int reg = start; reg < start + 8; reg++)
            {
                Write8(address + (position++ & 15), vu_.Byte(reg, byte++ & 15));
                Write8(address + (position++ & 15), vu_.Byte(reg, byte++ & 15));
            }
            break;
        }

    default:
        break;
    }
}

} // namespace usf::n64
