// SPDX-License-Identifier: MIT

// The VR4300's main loop, its integer instructions, and its loads and stores. COP0 and translation are in
// cpu_cop0.cpp and COP1 in cpu_fpu.cpp.

#include "n64/cpu.h"

#include <limits>

#include "xsflib/bytes.h"

#include "n64/system.h"

namespace usf::n64
{

namespace
{

uint64_t SignExtend32(uint64_t value)
{
    return static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(static_cast<uint32_t>(value))));
}

// The 128-bit product of two 64-bit values, as high and low halves.
void Multiply64(uint64_t a, uint64_t b, uint64_t& high, uint64_t& low)
{
    const uint64_t a_low = a & 0xFFFFFFFF;
    const uint64_t a_high = a >> 32;
    const uint64_t b_low = b & 0xFFFFFFFF;
    const uint64_t b_high = b >> 32;
    const uint64_t low_low = a_low * b_low;
    const uint64_t high_low = a_high * b_low;
    const uint64_t low_high = a_low * b_high;
    const uint64_t high_high = a_high * b_high;
    const uint64_t middle = (low_low >> 32) + (high_low & 0xFFFFFFFF) + (low_high & 0xFFFFFFFF);
    low = (middle << 32) | (low_low & 0xFFFFFFFF);
    high = high_high + (high_low >> 32) + (low_high >> 32) + (middle >> 32);
}

void MultiplySigned64(int64_t a, int64_t b, uint64_t& high, uint64_t& low)
{
    Multiply64(static_cast<uint64_t>(a), static_cast<uint64_t>(b), high, low);

    if (a < 0)
    {
        high -= static_cast<uint64_t>(b);
    }
    if (b < 0)
    {
        high -= static_cast<uint64_t>(a);
    }
}

} // namespace

Cpu::Cpu(System& system) : system_(system), now_(system.Clock())
{
    Reset();
}

uint32_t Cpu::Rs(uint32_t instruction)
{
    return (instruction >> 21) & 31;
}

uint32_t Cpu::Rt(uint32_t instruction)
{
    return (instruction >> 16) & 31;
}

uint32_t Cpu::Rd(uint32_t instruction)
{
    return (instruction >> 11) & 31;
}

uint32_t Cpu::Sa(uint32_t instruction)
{
    return (instruction >> 6) & 31;
}

uint64_t Cpu::SignedImmediate(uint32_t instruction)
{
    return static_cast<uint64_t>(static_cast<int64_t>(static_cast<int16_t>(instruction & 0xFFFF)));
}

uint64_t Cpu::UnsignedImmediate(uint32_t instruction)
{
    return instruction & 0xFFFF;
}

void Cpu::Reset()
{
    gpr_ = {};
    hi_ = 0;
    lo_ = 0;
    current_pc_ = 0xFFFFFFFFBFC00000ull;
    pc_ = current_pc_;
    next_pc_ = pc_ + 4;
    branch_pending_ = false;
    in_delay_slot_ = false;
    idle_loop_ = false;
    late_events_ = false;
    event_check_ = false;
    status_check_ = false;

    index_ = 0;
    entry_lo0_ = 0;
    entry_lo1_ = 0;
    context_ = 0x007FFFF0;
    page_mask_ = 0;
    wired_ = 0;
    // A console that has just been turned on reads all ones from BadVAddr, EPC and ErrorEPC, Context and XContext
    // that match that BadVAddr, and 3 from the coprocessor field of Cause. A reset doesn't change them.
    // n64-systemtest's startup test and PeterLemon's COP0 and exception tests record these values.
    bad_vaddr_ = ~uint64_t{0};
    count_offset_ = 0 - now_;
    entry_hi_ = 0;
    compare_ = 0;
    status_ = 0x34000000;
    cause_ = 0x30000000;
    epc_ = ~uint64_t{0};
    config_ = 0x7006E463;
    ll_addr_ = 0;
    watch_lo_ = 0;
    watch_hi_ = 0;
    x_context_ = 0x1FFFFFFF0;
    parity_error_ = 0;
    tag_lo_ = 0;
    error_epc_ = ~uint64_t{0};
    latch_ = 0;
    cop2_latch_ = 0;
    random_epoch_ = 0;
    ll_bit_ = false;
    tlb_ = {};
    tlb_pages_ = system_.GetOptions().timing == Timing::kLegacy;
    read_pages_.Clear();
    write_pages_.Clear();
    ip2_stale_ = false;
    count_updated_ = 0;

    fpr_ = {};
    fcr31_ = 0;
    legacy_rounding_ = 0;
    instructions_ = 0;
    interrupt_pending_ = false;
    fetch_page_ = ~uint64_t{0};
    translation_epoch_++;

    // The TLB's contents at power-on are undefined, but they don't map the low addresses: PeterLemon's TLB tests see
    // a refill exception for address 0x10. Each entry gets a page of kseg0, which is never translated, as libultra
    // does when it clears the TLB.
    for (int i = 0; i < 32; i++)
    {
        TlbEntry entry;
        entry.entry_hi = 0xFFFFFFFF80000000ull + static_cast<uint64_t>(i) * 0x2000;
        SetTlb(i, entry);
    }
}

uint64_t Cpu::Gpr(int index) const
{
    return gpr_[index];
}

void Cpu::SetGpr(int index, uint64_t value)
{
    if (index != 0)
    {
        gpr_[index] = value;
    }
}

uint64_t Cpu::Pc() const
{
    return pc_;
}

void Cpu::SetPc(uint64_t pc)
{
    pc_ = pc;
    next_pc_ = pc + 4;
    branch_pending_ = false;
}

uint64_t Cpu::Hi() const
{
    return hi_;
}

uint64_t Cpu::Lo() const
{
    return lo_;
}

void Cpu::SetHi(uint64_t value)
{
    hi_ = value;
}

void Cpu::SetLo(uint64_t value)
{
    lo_ = value;
}

uint64_t Cpu::InstructionCount() const
{
    return instructions_;
}

bool Cpu::InIdleLoop() const
{
    return idle_loop_;
}

bool Cpu::InDelaySlot() const
{
    return branch_pending_;
}

void Cpu::Run(uint64_t until)
{
    late_events_ = !system_.GetOptions().exact_events && system_.GetOptions().timing == Timing::kLegacy;
    if (late_events_)
    {
        RunLate(until);
        return;
    }

    const uint64_t ticks = system_.TicksPerInstruction();
    const bool& slice_break = system_.SliceBreakFlag();
    const bool breakpoints = system_.HasBreakpoints();
    while (now_ < until && !slice_break)
    {
        if (breakpoints && system_.ShouldBreak(pc_))
        {
            break;
        }

        Step();
        now_ += ticks;
        if (idle_loop_)
        {
            // Nothing can change until the next event, so skip to it. Random counts the skipped instructions.
            idle_loop_ = false;
            if (!interrupt_pending_ && now_ < until)
            {
                const uint64_t skipped = (until - now_ + ticks - 1) / ticks;
                instructions_ += skipped;
                now_ += skipped * ticks;
            }
        }
    }
}

void Cpu::RunLate(uint64_t until)
{
    // The legacy timing. Count includes each instruction while it runs. A device that the instruction reaches sees that
    // Count. An interrupt is taken only at a check point.
    const uint64_t ticks = system_.TicksPerInstruction();
    const bool& slice_break = system_.SliceBreakFlag();
    const bool breakpoints = system_.HasBreakpoints();
    const bool fast = !breakpoints && system_.ActiveCoverage() == nullptr;
    while (now_ < until && !slice_break)
    {
        if (fast && RunFast(until))
        {
            continue;
        }
        if (breakpoints && system_.ShouldBreak(pc_))
        {
            break;
        }

        const uint64_t before = now_;
        const bool delay_slot = branch_pending_;
        event_check_ = false;
        status_check_ = false;
        now_ += ticks;
        Step();
        if (idle_loop_)
        {
            // Count jumps to the next event. The branch then adds its 2 count units.
            idle_loop_ = false;
            const uint64_t next = system_.NextEvent();
            if (next != ~uint64_t{0} && next > before)
            {
                const uint64_t target = next + ticks;
                instructions_ += (target - now_) / ticks;
                now_ = target;
            }
        }

        // Each check point handles one due event at most, and brings Count up to date.
        if (status_check_)
        {
            system_.LateCheck(now_ - ticks);
            count_updated_ = now_;
        }
        if (delay_slot || event_check_)
        {
            system_.LateCheck(now_);
            count_updated_ = now_;
        }
    }
}

void Cpu::RequestEventCheck()
{
    event_check_ = true;
}

bool Cpu::InterruptPending() const
{
    return interrupt_pending_;
}

void Cpu::EnterInterrupt()
{
    // Taking an interrupt adds one instruction's time to Count. With this charge, the output follows the legacy
    // players' output for longer than without it.
    now_ += system_.TicksPerInstruction();
    TakeInterrupt();
}

void Cpu::Step()
{
    // Without exact events, interrupts are taken only at check points.
    if (interrupt_pending_ && !late_events_)
    {
        TakeInterrupt();
        return;
    }

    current_pc_ = pc_;
    in_delay_slot_ = branch_pending_;
    branch_pending_ = false;
    instructions_++;

    uint32_t instruction = 0;
    if (!Fetch(instruction))
    {
        return;
    }

    pc_ = next_pc_;
    next_pc_ += 4;
    Execute(instruction);
    gpr_[0] = 0;
}

bool Cpu::Fetch(uint32_t& instruction)
{
    const uint64_t pc = current_pc_;
    if ((pc & 3) != 0)
    {
        RaiseAddressError(pc, Access::kFetch);
        return false;
    }

    if ((pc & ~uint64_t{0xFFF}) == fetch_page_ && fetch_epoch_ == translation_epoch_)
    {
        const uint32_t paddr = fetch_page_paddr_ | static_cast<uint32_t>(pc & 0xFFF);
        instruction = xsflib::LoadBe32(system_.RdramData() + paddr);
        if (Coverage* coverage = system_.ActiveCoverage())
        {
            coverage->ReadRdram(paddr, 4);
        }

        return true;
    }

    uint32_t paddr = 0;
    if (!Translate(pc, Access::kFetch, paddr))
    {
        return false;
    }

    if (paddr < system_.DirectRdramSize())
    {
        fetch_page_ = pc & ~uint64_t{0xFFF};
        fetch_page_paddr_ = paddr & ~0xFFFu;
        fetch_epoch_ = translation_epoch_;
        instruction = xsflib::LoadBe32(system_.RdramData() + paddr);
        if (Coverage* coverage = system_.ActiveCoverage())
        {
            coverage->ReadRdram(paddr, 4);
        }

        return true;
    }

    instruction = system_.ReadBus32(paddr);

    return true;
}

template <typename T>
bool Cpu::Load(uint64_t vaddr, T& value)
{
    if ((vaddr & (sizeof(T) - 1)) != 0)
    {
        RaiseAddressError(vaddr, Access::kLoad);
        return false;
    }

    uint32_t paddr = 0;
    if (!Translate(vaddr, Access::kLoad, paddr))
    {
        return false;
    }

    LoadPhysical(paddr, value);

    return true;
}

template <typename T>
void Cpu::LoadPhysical(uint32_t paddr, T& value)
{
    if (paddr < system_.DirectRdramSize())
    {
        const uint8_t* p = system_.RdramData() + paddr;
        if constexpr (sizeof(T) == 1)
        {
            value = *p;
        }
        else if constexpr (sizeof(T) == 2)
        {
            value = xsflib::LoadBe16(p);
        }
        else if constexpr (sizeof(T) == 4)
        {
            value = xsflib::LoadBe32(p);
        }
        else
        {
            value = xsflib::LoadBe64(p);
        }

        if (Coverage* coverage = system_.ActiveCoverage())
        {
            coverage->ReadRdram(paddr, sizeof(T));
        }

        return;
    }

    if constexpr (sizeof(T) == 8)
    {
        value = system_.ReadBus64(paddr);
    }
    else
    {
        const uint32_t word = sizeof(T) == 4 ? system_.ReadBus32(paddr) : system_.ReadBusPartial32(paddr);
        const uint32_t shift = 8 * (4 - sizeof(T) - (paddr & 3));
        value = static_cast<T>(word >> shift);
    }
}

template <typename T>
bool Cpu::Store(uint64_t vaddr, uint64_t value)
{
    if ((vaddr & (sizeof(T) - 1)) != 0)
    {
        RaiseAddressError(vaddr, Access::kStore);
        return false;
    }

    uint32_t paddr = 0;
    if (!Translate(vaddr, Access::kStore, paddr))
    {
        return false;
    }

    system_.WriteBus(paddr, value, sizeof(T));

    return true;
}

template bool Cpu::Load<uint8_t>(uint64_t, uint8_t&);
template void Cpu::LoadPhysical<uint8_t>(uint32_t, uint8_t&);
template bool Cpu::Load<uint16_t>(uint64_t, uint16_t&);
template void Cpu::LoadPhysical<uint16_t>(uint32_t, uint16_t&);
template bool Cpu::Load<uint32_t>(uint64_t, uint32_t&);
template void Cpu::LoadPhysical<uint32_t>(uint32_t, uint32_t&);
template bool Cpu::Load<uint64_t>(uint64_t, uint64_t&);
template void Cpu::LoadPhysical<uint64_t>(uint32_t, uint64_t&);
template bool Cpu::Store<uint8_t>(uint64_t, uint64_t);
template bool Cpu::Store<uint16_t>(uint64_t, uint64_t);
template bool Cpu::Store<uint32_t>(uint64_t, uint64_t);
template bool Cpu::Store<uint64_t>(uint64_t, uint64_t);

uint64_t Cpu::BranchTarget(uint32_t instruction) const
{
    return current_pc_ + 4 + (SignedImmediate(instruction) << 2);
}

void Cpu::Branch(bool taken, uint64_t target)
{
    // The next instruction is in the branch's delay slot whether or not the branch is taken.
    branch_pending_ = true;
    if (!taken)
    {
        return;
    }

    next_pc_ = target;

    // A jump to itself with a NOP in its delay slot is an idle loop.
    if (target == current_pc_)
    {
        if (auto paddr = TranslateQuiet(current_pc_ + 4, false))
        {
            if (*paddr < system_.DirectRdramSize() && xsflib::LoadBe32(system_.RdramData() + *paddr) == 0)
            {
                idle_loop_ = true;
            }
        }
    }
}

void Cpu::BranchLikely(bool taken, uint64_t target)
{
    if (taken)
    {
        Branch(true, target);
        return;
    }

    // A likely branch that isn't taken skips its delay slot. The branch ends here. Due events are checked after it. The
    // legacy timing counts the skipped slot.
    pc_ = next_pc_;
    next_pc_ += 4;
    event_check_ = true;
    if (late_events_)
    {
        now_ += system_.TicksPerInstruction();
    }
}

bool Cpu::Allows64Bit() const
{
    const bool kernel = (status_ & (kStatusExl | kStatusErl)) != 0 || ((status_ >> 3) & 3) == 0;
    if (kernel)
    {
        return true;
    }
    if (((status_ >> 3) & 3) == 1)
    {
        return (status_ & kStatusSx) != 0;
    }

    return (status_ & kStatusUx) != 0;
}

void Cpu::Execute(uint32_t instruction)
{
    const uint32_t op = instruction >> 26;
    const uint32_t rs = Rs(instruction);
    const uint32_t rt = Rt(instruction);
    switch (op)
    {
    case 0x00:
        ExecuteSpecial(instruction);
        break;

    case 0x01:
        ExecuteRegimm(instruction);
        break;

    case 0x02:
        Branch(true, ((current_pc_ + 4) & 0xFFFFFFFFF0000000ull) | ((instruction & 0x03FFFFFF) << 2));
        break;

    case 0x03:
        gpr_[31] = pc_ + 4;
        Branch(true, ((current_pc_ + 4) & 0xFFFFFFFFF0000000ull) | ((instruction & 0x03FFFFFF) << 2));
        break;

    case 0x04:
        Branch(gpr_[rs] == gpr_[rt], BranchTarget(instruction));
        break;

    case 0x05:
        Branch(gpr_[rs] != gpr_[rt], BranchTarget(instruction));
        break;

    case 0x06:
        Branch(static_cast<int64_t>(gpr_[rs]) <= 0, BranchTarget(instruction));
        break;

    case 0x07:
        Branch(static_cast<int64_t>(gpr_[rs]) > 0, BranchTarget(instruction));
        break;

    case 0x08:
        {
            // ADDI traps on signed overflow.
            const uint32_t a = static_cast<uint32_t>(gpr_[rs]);
            const uint32_t b = static_cast<uint32_t>(SignedImmediate(instruction));
            const uint32_t sum = a + b;
            if (((~(a ^ b) & (a ^ sum)) & 0x80000000u) != 0)
            {
                RaiseException(ExceptionCode::kOverflow);
                break;
            }

            gpr_[rt] = SignExtend32(sum);
            break;
        }

    case 0x09:
        gpr_[rt] = SignExtend32(gpr_[rs] + SignedImmediate(instruction));
        break;

    case 0x0A:
        gpr_[rt] = static_cast<int64_t>(gpr_[rs]) < static_cast<int64_t>(SignedImmediate(instruction)) ? 1 : 0;
        break;

    case 0x0B:
        gpr_[rt] = gpr_[rs] < SignedImmediate(instruction) ? 1 : 0;
        break;

    case 0x0C:
        gpr_[rt] = gpr_[rs] & UnsignedImmediate(instruction);
        break;

    case 0x0D:
        gpr_[rt] = gpr_[rs] | UnsignedImmediate(instruction);
        break;

    case 0x0E:
        gpr_[rt] = gpr_[rs] ^ UnsignedImmediate(instruction);
        break;

    case 0x0F:
        gpr_[rt] = SignExtend32(UnsignedImmediate(instruction) << 16);
        break;

    case 0x10:
        ExecuteCop0(instruction);
        break;

    case 0x11:
        ExecuteCop1(instruction);
        break;

    case 0x12:
        // COP2: the VR4300 has no coprocessor 2. When it's marked usable, the moves to and from it use one 64-bit
        // latch, and its other instructions are reserved.
        if ((status_ & kStatusCu2) == 0)
        {
            RaiseException(ExceptionCode::kCoprocessorUnusable, 2);
            break;
        }

        switch (rs)
        {
        case 0x00: // MFC2
        case 0x02: // CFC2
            gpr_[rt] = SignExtend32(static_cast<uint32_t>(cop2_latch_));
            break;
        case 0x01: // DMFC2
            gpr_[rt] = cop2_latch_;
            break;
        case 0x04: // MTC2
        case 0x05: // DMTC2
        case 0x06: // CTC2
            cop2_latch_ = gpr_[rt];
            break;
        default:
            RaiseException(ExceptionCode::kReservedInstruction, 2);
            break;
        }
        break;

    case 0x14:
        BranchLikely(gpr_[rs] == gpr_[rt], BranchTarget(instruction));
        break;

    case 0x15:
        BranchLikely(gpr_[rs] != gpr_[rt], BranchTarget(instruction));
        break;

    case 0x16:
        BranchLikely(static_cast<int64_t>(gpr_[rs]) <= 0, BranchTarget(instruction));
        break;

    case 0x17:
        BranchLikely(static_cast<int64_t>(gpr_[rs]) > 0, BranchTarget(instruction));
        break;

    case 0x18:
        {
            if (!Allows64Bit())
            {
                RaiseException(ExceptionCode::kReservedInstruction);
                break;
            }

            const uint64_t a = gpr_[rs];
            const uint64_t b = SignedImmediate(instruction);
            const uint64_t sum = a + b;
            if (((~(a ^ b) & (a ^ sum)) >> 63) != 0)
            {
                RaiseException(ExceptionCode::kOverflow);
                break;
            }

            gpr_[rt] = sum;
            break;
        }

    case 0x19:
        if (!Allows64Bit())
        {
            RaiseException(ExceptionCode::kReservedInstruction);
            break;
        }

        gpr_[rt] = gpr_[rs] + SignedImmediate(instruction);
        break;

    case 0x2F:
        // CACHE: no cache is modelled. Outside kernel mode it needs COP0.
        if (!((status_ & (kStatusExl | kStatusErl)) != 0 || ((status_ >> 3) & 3) == 0) && (status_ & kStatusCu0) == 0)
        {
            RaiseException(ExceptionCode::kCoprocessorUnusable, 0);
        }
        break;

    default:
        if (op >= 0x1A && op != 0x1C && op != 0x1D && op != 0x1E && op != 0x1F && op != 0x33 && op != 0x3B &&
            op != 0x32 && op != 0x36 && op != 0x3A && op != 0x3E)
        {
            ExecuteLoadStore(instruction);
            break;
        }
        if (op == 0x32 || op == 0x36 || op == 0x3A || op == 0x3E)
        {
            // LWC2, LDC2, SWC2 and SDC2.
            if ((status_ & kStatusCu2) == 0)
            {
                RaiseException(ExceptionCode::kCoprocessorUnusable, 2);
            }
            else
            {
                RaiseException(ExceptionCode::kReservedInstruction, 2);
            }
            break;
        }

        RaiseException(ExceptionCode::kReservedInstruction);
        break;
    }
}

void Cpu::ExecuteSpecial(uint32_t instruction)
{
    const uint32_t rs = Rs(instruction);
    const uint32_t rt = Rt(instruction);
    const uint32_t rd = Rd(instruction);
    const uint32_t sa = Sa(instruction);
    const uint64_t s = gpr_[rs];
    const uint64_t t = gpr_[rt];
    const uint32_t funct = instruction & 0x3F;

    // The 64-bit operations are reserved in 32-bit user and supervisor modes.
    const bool doubleword = funct == 0x14 || funct == 0x16 || funct == 0x17 || (funct >= 0x1C && funct <= 0x1F) ||
                            (funct >= 0x2C && funct <= 0x2F) || funct >= 0x38;
    if (doubleword && funct != 0x39 && funct != 0x3D && !Allows64Bit())
    {
        RaiseException(ExceptionCode::kReservedInstruction);
        return;
    }

    switch (funct)
    {
    case 0x00:
        gpr_[rd] = SignExtend32(static_cast<uint32_t>(t) << sa);
        break;

    case 0x02:
        gpr_[rd] = SignExtend32(static_cast<uint32_t>(t) >> sa);
        break;

    case 0x03:
        gpr_[rd] = SignExtend32(static_cast<uint64_t>(static_cast<int64_t>(t) >> sa));
        break;

    case 0x04:
        gpr_[rd] = SignExtend32(static_cast<uint32_t>(t) << (s & 31));
        break;

    case 0x06:
        gpr_[rd] = SignExtend32(static_cast<uint32_t>(t) >> (s & 31));
        break;

    case 0x07:
        gpr_[rd] = SignExtend32(static_cast<uint64_t>(static_cast<int64_t>(t) >> (s & 31)));
        break;

    case 0x08:
        Branch(true, s);
        break;

    case 0x09:
        gpr_[rd] = pc_ + 4;
        Branch(true, s);
        break;

    case 0x0C:
        RaiseException(ExceptionCode::kSyscall);
        break;

    case 0x0D:
        if (system_.GetOptions().timing == Timing::kLegacy)
        {
            // The legacy players raise nothing. Old rips use BREAK as a wait. The legacy timing moves Count to the time
            // of the first event in the queue, even back to it. Count isn't brought up to date first. The instructions
            // since Count's last update are therefore still to add.
            const uint64_t first = system_.FirstEventTime();
            if (late_events_ && first != ~uint64_t{0})
            {
                now_ = first + (now_ - count_updated_);
                count_updated_ = first;
            }
            break;
        }

        RaiseException(ExceptionCode::kBreakpoint);
        break;

    case 0x0F:
        break; // SYNC.

    case 0x10:
        gpr_[rd] = hi_;
        break;

    case 0x11:
        hi_ = s;
        break;

    case 0x12:
        gpr_[rd] = lo_;
        break;

    case 0x13:
        lo_ = s;
        break;

    case 0x14:
        gpr_[rd] = t << (s & 63);
        break;

    case 0x16:
        gpr_[rd] = t >> (s & 63);
        break;

    case 0x17:
        gpr_[rd] = static_cast<uint64_t>(static_cast<int64_t>(t) >> (s & 63));
        break;

    case 0x18:
        {
            // The hardware multiplies all 64 bits of rs by the low 35 bits of rt. Operands that aren't sign-extended
            // 32-bit values show it.
            const uint64_t t35 = static_cast<uint64_t>(static_cast<int64_t>(t << 29) >> 29);
            const uint64_t product = s * t35;
            lo_ = SignExtend32(product);
            hi_ = SignExtend32(product >> 32);
            break;
        }

    case 0x19:
        {
            const uint64_t product = static_cast<uint64_t>(static_cast<uint32_t>(s)) * static_cast<uint32_t>(t);
            lo_ = SignExtend32(product);
            hi_ = SignExtend32(product >> 32);
            break;
        }

    case 0x1A:
        {
            const int32_t n = static_cast<int32_t>(s);
            const int32_t d = static_cast<int32_t>(t);
            if (d == 0)
            {
                lo_ = n >= 0 ? ~uint64_t{0} : 1;
                hi_ = SignExtend32(static_cast<uint32_t>(n));
            }
            else if (n == std::numeric_limits<int32_t>::min() && d == -1)
            {
                lo_ = SignExtend32(static_cast<uint32_t>(n));
                hi_ = 0;
            }
            else
            {
                lo_ = SignExtend32(static_cast<uint32_t>(n / d));
                hi_ = SignExtend32(static_cast<uint32_t>(n % d));
            }
            break;
        }

    case 0x1B:
        {
            const uint32_t n = static_cast<uint32_t>(s);
            const uint32_t d = static_cast<uint32_t>(t);
            if (d == 0)
            {
                lo_ = ~uint64_t{0};
                hi_ = SignExtend32(n);
            }
            else
            {
                lo_ = SignExtend32(n / d);
                hi_ = SignExtend32(n % d);
            }
            break;
        }

    case 0x1C:
        MultiplySigned64(static_cast<int64_t>(s), static_cast<int64_t>(t), hi_, lo_);
        break;

    case 0x1D:
        Multiply64(s, t, hi_, lo_);
        break;

    case 0x1E:
        {
            const int64_t n = static_cast<int64_t>(s);
            const int64_t d = static_cast<int64_t>(t);
            if (d == 0)
            {
                lo_ = n >= 0 ? ~uint64_t{0} : 1;
                hi_ = s;
            }
            else if (n == std::numeric_limits<int64_t>::min() && d == -1)
            {
                lo_ = s;
                hi_ = 0;
            }
            else
            {
                lo_ = static_cast<uint64_t>(n / d);
                hi_ = static_cast<uint64_t>(n % d);
            }
            break;
        }

    case 0x1F:
        if (t == 0)
        {
            lo_ = ~uint64_t{0};
            hi_ = s;
        }
        else
        {
            lo_ = s / t;
            hi_ = s % t;
        }
        break;

    case 0x20:
        {
            const uint32_t a = static_cast<uint32_t>(s);
            const uint32_t b = static_cast<uint32_t>(t);
            const uint32_t sum = a + b;
            if (((~(a ^ b) & (a ^ sum)) & 0x80000000u) != 0)
            {
                RaiseException(ExceptionCode::kOverflow);
                break;
            }

            gpr_[rd] = SignExtend32(sum);
            break;
        }

    case 0x21:
        gpr_[rd] = SignExtend32(s + t);
        break;

    case 0x22:
        {
            const uint32_t a = static_cast<uint32_t>(s);
            const uint32_t b = static_cast<uint32_t>(t);
            const uint32_t difference = a - b;
            if ((((a ^ b) & (a ^ difference)) & 0x80000000u) != 0)
            {
                RaiseException(ExceptionCode::kOverflow);
                break;
            }

            gpr_[rd] = SignExtend32(difference);
            break;
        }

    case 0x23:
        gpr_[rd] = SignExtend32(s - t);
        break;

    case 0x24:
        gpr_[rd] = s & t;
        break;

    case 0x25:
        gpr_[rd] = s | t;
        break;

    case 0x26:
        gpr_[rd] = s ^ t;
        break;

    case 0x27:
        gpr_[rd] = ~(s | t);
        break;

    case 0x2A:
        gpr_[rd] = static_cast<int64_t>(s) < static_cast<int64_t>(t) ? 1 : 0;
        break;

    case 0x2B:
        gpr_[rd] = s < t ? 1 : 0;
        break;

    case 0x2C:
        {
            const uint64_t sum = s + t;
            if (((~(s ^ t) & (s ^ sum)) >> 63) != 0)
            {
                RaiseException(ExceptionCode::kOverflow);
                break;
            }

            gpr_[rd] = sum;
            break;
        }

    case 0x2D:
        gpr_[rd] = s + t;
        break;

    case 0x2E:
        {
            const uint64_t difference = s - t;
            if ((((s ^ t) & (s ^ difference)) >> 63) != 0)
            {
                RaiseException(ExceptionCode::kOverflow);
                break;
            }

            gpr_[rd] = difference;
            break;
        }

    case 0x2F:
        gpr_[rd] = s - t;
        break;

    case 0x30:
        if (static_cast<int64_t>(s) >= static_cast<int64_t>(t))
        {
            RaiseException(ExceptionCode::kTrap);
        }
        break;

    case 0x31:
        if (s >= t)
        {
            RaiseException(ExceptionCode::kTrap);
        }
        break;

    case 0x32:
        if (static_cast<int64_t>(s) < static_cast<int64_t>(t))
        {
            RaiseException(ExceptionCode::kTrap);
        }
        break;

    case 0x33:
        if (s < t)
        {
            RaiseException(ExceptionCode::kTrap);
        }
        break;

    case 0x34:
        if (s == t)
        {
            RaiseException(ExceptionCode::kTrap);
        }
        break;

    case 0x36:
        if (s != t)
        {
            RaiseException(ExceptionCode::kTrap);
        }
        break;

    case 0x38:
        gpr_[rd] = t << sa;
        break;

    case 0x3A:
        gpr_[rd] = t >> sa;
        break;

    case 0x3B:
        gpr_[rd] = static_cast<uint64_t>(static_cast<int64_t>(t) >> sa);
        break;

    case 0x3C:
        gpr_[rd] = t << (sa + 32);
        break;

    case 0x3E:
        gpr_[rd] = t >> (sa + 32);
        break;

    case 0x3F:
        gpr_[rd] = static_cast<uint64_t>(static_cast<int64_t>(t) >> (sa + 32));
        break;

    default:
        RaiseException(ExceptionCode::kReservedInstruction);
        break;
    }
}

void Cpu::ExecuteRegimm(uint32_t instruction)
{
    const uint32_t rs = Rs(instruction);
    const int64_t s = static_cast<int64_t>(gpr_[rs]);
    const int64_t immediate = static_cast<int64_t>(SignedImmediate(instruction));
    const uint64_t target = BranchTarget(instruction);
    switch (Rt(instruction))
    {
    case 0x00:
        Branch(s < 0, target);
        break;

    case 0x01:
        Branch(s >= 0, target);
        break;

    case 0x02:
        BranchLikely(s < 0, target);
        break;

    case 0x03:
        BranchLikely(s >= 0, target);
        break;

    case 0x08:
        if (s >= immediate)
        {
            RaiseException(ExceptionCode::kTrap);
        }
        break;

    case 0x09:
        if (static_cast<uint64_t>(s) >= static_cast<uint64_t>(immediate))
        {
            RaiseException(ExceptionCode::kTrap);
        }
        break;

    case 0x0A:
        if (s < immediate)
        {
            RaiseException(ExceptionCode::kTrap);
        }
        break;

    case 0x0B:
        if (static_cast<uint64_t>(s) < static_cast<uint64_t>(immediate))
        {
            RaiseException(ExceptionCode::kTrap);
        }
        break;

    case 0x0C:
        if (s == immediate)
        {
            RaiseException(ExceptionCode::kTrap);
        }
        break;

    case 0x0E:
        if (s != immediate)
        {
            RaiseException(ExceptionCode::kTrap);
        }
        break;

    case 0x10:
        gpr_[31] = pc_ + 4;
        Branch(s < 0, target);
        break;

    case 0x11:
        gpr_[31] = pc_ + 4;
        Branch(s >= 0, target);
        break;

    case 0x12:
        gpr_[31] = pc_ + 4;
        BranchLikely(s < 0, target);
        break;

    case 0x13:
        gpr_[31] = pc_ + 4;
        BranchLikely(s >= 0, target);
        break;

    default:
        RaiseException(ExceptionCode::kReservedInstruction);
        break;
    }
}

void Cpu::ExecuteLoadStore(uint32_t instruction)
{
    const uint32_t op = instruction >> 26;
    const uint32_t rt = Rt(instruction);
    const uint64_t vaddr = gpr_[Rs(instruction)] + SignedImmediate(instruction);

    // The 64-bit loads and stores are reserved in 32-bit user and supervisor modes.
    const bool doubleword =
        op == 0x1A || op == 0x1B || op == 0x2C || op == 0x2D || op == 0x34 || op == 0x37 || op == 0x3C || op == 0x3F;
    if (doubleword && !Allows64Bit())
    {
        RaiseException(ExceptionCode::kReservedInstruction);
        return;
    }

    switch (op)
    {
    case 0x20:
        {
            uint8_t value = 0;
            if (Load(vaddr, value))
            {
                gpr_[rt] = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int8_t>(value)));
            }
            break;
        }

    case 0x21:
        {
            uint16_t value = 0;
            if (Load(vaddr, value))
            {
                gpr_[rt] = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int16_t>(value)));
            }
            break;
        }

    case 0x23:
        {
            uint32_t value = 0;
            if (Load(vaddr, value))
            {
                gpr_[rt] = SignExtend32(value);
            }
            break;
        }

    case 0x24:
        {
            uint8_t value = 0;
            if (Load(vaddr, value))
            {
                gpr_[rt] = value;
            }
            break;
        }

    case 0x25:
        {
            uint16_t value = 0;
            if (Load(vaddr, value))
            {
                gpr_[rt] = value;
            }
            break;
        }

    case 0x27:
        {
            uint32_t value = 0;
            if (Load(vaddr, value))
            {
                gpr_[rt] = value;
            }
            break;
        }

    case 0x37:
        {
            uint64_t value = 0;
            if (Load(vaddr, value))
            {
                gpr_[rt] = value;
            }
            break;
        }

    case 0x22:
    case 0x26:
        {
            // LWL and LWR merge the bytes of an aligned word into the register. LWL always sign-extends. LWR does only
            // when it loads the whole word.
            // The exception for an unmapped address reports the address itself, not the aligned word's.
            uint32_t paddr = 0;
            if (!Translate(vaddr, Access::kLoad, paddr))
            {
                break;
            }

            uint32_t word = 0;
            LoadPhysical(paddr & ~3u, word);

            const uint32_t offset = static_cast<uint32_t>(vaddr & 3);
            const uint32_t old = static_cast<uint32_t>(gpr_[rt]);
            if (op == 0x22)
            {
                const uint32_t shift = 8 * offset;
                const uint32_t keep = offset == 0 ? 0 : (0xFFFFFFFFu >> (32 - shift));
                gpr_[rt] = SignExtend32((old & keep) | (word << shift));
            }
            else
            {
                const uint32_t shift = 8 * (3 - offset);
                const uint32_t keep = offset == 3 ? 0 : ~(0xFFFFFFFFu >> shift);
                const uint32_t merged = (old & keep) | (word >> shift);
                gpr_[rt] = offset == 3 ? SignExtend32(merged) : ((gpr_[rt] & 0xFFFFFFFF00000000ull) | merged);
            }
            break;
        }

    case 0x1A:
    case 0x1B:
        {
            // LDL and LDR.
            uint32_t paddr = 0;
            if (!Translate(vaddr, Access::kLoad, paddr))
            {
                break;
            }

            uint64_t doubleword_value = 0;
            LoadPhysical(paddr & ~7u, doubleword_value);

            const uint32_t offset = static_cast<uint32_t>(vaddr & 7);
            const uint64_t old = gpr_[rt];
            if (op == 0x1A)
            {
                const uint32_t shift = 8 * offset;
                const uint64_t keep = offset == 0 ? 0 : (~uint64_t{0} >> (64 - shift));
                gpr_[rt] = (old & keep) | (doubleword_value << shift);
            }
            else
            {
                const uint32_t shift = 8 * (7 - offset);
                const uint64_t keep = offset == 7 ? 0 : ~(~uint64_t{0} >> shift);
                gpr_[rt] = (old & keep) | (doubleword_value >> shift);
            }
            break;
        }

    case 0x30:
    case 0x34:
        {
            // LL and LLD record the physical address and set the link bit.
            uint32_t paddr = 0;
            const bool word = op == 0x30;
            if ((vaddr & (word ? 3 : 7)) != 0)
            {
                RaiseAddressError(vaddr, Access::kLoad);
                break;
            }
            if (!Translate(vaddr, Access::kLoad, paddr))
            {
                break;
            }

            if (word)
            {
                uint32_t value = 0;
                if (!Load(vaddr, value))
                {
                    break;
                }

                gpr_[rt] = SignExtend32(value);
            }
            else
            {
                uint64_t value = 0;
                if (!Load(vaddr, value))
                {
                    break;
                }

                gpr_[rt] = value;
            }

            ll_addr_ = paddr >> 4;
            ll_bit_ = true;
            break;
        }

    case 0x28:
        Store<uint8_t>(vaddr, gpr_[rt]);
        break;

    case 0x29:
        Store<uint16_t>(vaddr, gpr_[rt]);
        break;

    case 0x2B:
        Store<uint32_t>(vaddr, gpr_[rt]);
        break;

    case 0x3F:
        Store<uint64_t>(vaddr, gpr_[rt]);
        break;

    case 0x38:
        // SC stores only while the link bit holds. It writes 1 to rt on success and 0 otherwise.
        if (ll_bit_)
        {
            if (Store<uint32_t>(vaddr, gpr_[rt]))
            {
                gpr_[rt] = 1;
            }
        }
        else
        {
            gpr_[rt] = 0;
        }
        break;

    case 0x3C:
        if (ll_bit_)
        {
            if (Store<uint64_t>(vaddr, gpr_[rt]))
            {
                gpr_[rt] = 1;
            }
        }
        else
        {
            gpr_[rt] = 0;
        }
        break;

    case 0x2A:
    case 0x2E:
    case 0x2C:
    case 0x2D:
        {
            // SWL, SWR, SDL and SDR store part of the register into an aligned word or doubleword, as a series of
            // naturally aligned stores.
            const bool is_double = op == 0x2C || op == 0x2D;
            const uint32_t size = is_double ? 8 : 4;
            const uint32_t offset = static_cast<uint32_t>(vaddr & (size - 1));
            const bool left = op == 0x2A || op == 0x2C;
            const uint64_t value = is_double ? gpr_[rt] : (gpr_[rt] & 0xFFFFFFFF);

            // The bytes of the block to write, and the block's contents for them.
            uint32_t first = 0;
            uint32_t last = 0;
            uint64_t block = 0;
            if (left)
            {
                first = offset;
                last = size - 1;
                block = offset == 0 ? value : value >> (8 * offset);
            }
            else
            {
                first = 0;
                last = offset;
                block = offset == size - 1 ? value : value << (8 * (size - 1 - offset));
            }

            // The exception for an unmapped address reports the address itself, not the aligned block's.
            uint32_t paddr = 0;
            if (!Translate(vaddr, Access::kStore, paddr))
            {
                break;
            }
            paddr &= ~(size - 1);

            uint32_t position = first;
            while (position <= last)
            {
                // The largest aligned piece that starts here and fits.
                uint32_t piece = size;
                while (piece > 1 && ((position & (piece - 1)) != 0 || position + piece - 1 > last))
                {
                    piece /= 2;
                }

                const uint32_t shift = 8 * (size - position - piece);
                const uint64_t mask = piece == 8 ? ~uint64_t{0} : ((uint64_t{1} << (8 * piece)) - 1);
                system_.WriteBus(paddr + position, (block >> shift) & mask, static_cast<int>(piece));
                position += piece;
            }
            break;
        }

    case 0x31:
    case 0x35:
    case 0x39:
    case 0x3D:
        ExecuteCop1(instruction);
        break;

    default:
        RaiseException(ExceptionCode::kReservedInstruction);
        break;
    }
}

} // namespace usf::n64
