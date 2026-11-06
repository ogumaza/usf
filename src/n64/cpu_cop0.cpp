// SPDX-License-Identifier: MIT

// The VR4300's COP0: its registers, the TLB, exceptions and interrupts, and address translation.

#include "n64/cpu.h"
#include "n64/system.h"

namespace usf::n64
{

namespace
{

constexpr uint32_t kIndexMask = 0x8000003F;
constexpr uint32_t kEntryLoMask = 0x3FFFFFFF;
constexpr uint32_t kPageMaskMask = 0x01FFE000;
constexpr uint64_t kEntryHiMask = 0xC00000FFFFFFE0FFull;
constexpr uint64_t kVpn2Mask = 0x000000FFFFFFE000ull;
constexpr uint32_t kStatusWriteMask = 0xFF57FFFF;
constexpr uint32_t kConfigWriteMask = 0x0F00800F;
constexpr uint32_t kCauseExceptionMask = 0x7C;
constexpr uint32_t kCauseCoprocessorMask = 0x30000000;
constexpr uint32_t kCauseSoftwareMask = 0x300;
constexpr uint32_t kPrId = 0x00000B22;
constexpr uint64_t kVectorBase = 0xFFFFFFFF80000000ull;
constexpr uint64_t kVectorBaseBoot = 0xFFFFFFFFBFC00200ull;

uint64_t SignExtend32(uint64_t value)
{
    return static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(static_cast<uint32_t>(value))));
}

} // namespace

uint64_t Cpu::Cop0(int index) const
{
    return ReadCop0(index);
}

void Cpu::SetCop0(int index, uint64_t value)
{
    switch (index)
    {
    case kCop0Random:
        // Random counts down from 31 with each instruction. Set the point it started from so that it reads `value`.
        if (wired_ <= 31 && value >= wired_ && value <= 31)
        {
            random_epoch_ = instructions_ - (31 - value);
        }
        break;

    case kCop0BadVAddr:
        bad_vaddr_ = value;
        break;

    case kCop0Context:
        context_ = value & 0xFFFFFFFFFFFFFFF0ull;
        break;

    case kCop0XContext:
        x_context_ = value & 0xFFFFFFFFFFFFFFF0ull;
        break;

    case kCop0Cause:
        cause_ = static_cast<uint32_t>(value) & 0xB000FF7C;
        UpdateInterruptPending();
        break;

    case kCop0Wired:
        wired_ = static_cast<uint32_t>(value) & 0x3F;
        break;

    default:
        WriteCop0(index, value);
        break;
    }
}

uint32_t Cpu::Count() const
{
    return static_cast<uint32_t>((now_ + count_offset_) >> 1);
}

uint32_t Cpu::Random() const
{
    const uint64_t steps = instructions_ - random_epoch_;
    if (wired_ > 31)
    {
        return static_cast<uint32_t>((31 - steps) & 63);
    }

    const uint64_t range = 32 - wired_;
    return 31 - static_cast<uint32_t>(steps % range);
}

uint64_t Cpu::ReadCop0(int index) const
{
    switch (index)
    {
    case kCop0Index:
        return index_;
    case kCop0Random:
        return Random();
    case kCop0EntryLo0:
        return entry_lo0_;
    case kCop0EntryLo1:
        return entry_lo1_;
    case kCop0Context:
        return context_;
    case kCop0PageMask:
        return page_mask_;
    case kCop0Wired:
        return wired_;
    case kCop0BadVAddr:
        return bad_vaddr_;
    case kCop0Count:
        return Count();
    case kCop0EntryHi:
        return entry_hi_;
    case kCop0Compare:
        return compare_;
    case kCop0Status:
        return status_;
    case kCop0Cause:
        return cause_;
    case kCop0Epc:
        return epc_;
    case kCop0PrId:
        return kPrId;
    case kCop0Config:
        return config_;
    case kCop0LlAddr:
        return ll_addr_;
    case kCop0WatchLo:
        return watch_lo_;
    case kCop0WatchHi:
        return watch_hi_;
    case kCop0XContext:
        return x_context_;
    case kCop0ParityError:
        return parity_error_;
    case kCop0CacheError:
        return 0;
    case kCop0TagLo:
        return tag_lo_;
    case kCop0TagHi:
        return 0;
    case kCop0ErrorEpc:
        return error_epc_;
    default:
        // The registers that don't exist read the last value written to any COP0 register.
        return latch_;
    }
}

void Cpu::WriteCop0(int index, uint64_t value)
{
    latch_ = value;
    const uint32_t low = static_cast<uint32_t>(value);
    switch (index)
    {
    case kCop0Index:
        index_ = low & kIndexMask;
        break;

    case kCop0EntryLo0:
        entry_lo0_ = low & kEntryLoMask;
        break;

    case kCop0EntryLo1:
        entry_lo1_ = low & kEntryLoMask;
        break;

    case kCop0Context:
        context_ = (context_ & 0x7FFFF0) | (value & 0xFFFFFFFFFF800000ull);
        break;

    case kCop0PageMask:
        page_mask_ = low & kPageMaskMask;
        break;

    case kCop0Wired:
        wired_ = low & 0x3F;
        random_epoch_ = instructions_;
        break;

    case kCop0Count:
        WriteCount(low);
        break;

    case kCop0EntryHi:
        entry_hi_ = value & kEntryHiMask;
        translation_epoch_++;
        break;

    case kCop0Compare:
        WriteCompare(low);
        break;

    case kCop0Status:
        WriteStatus(low);
        break;

    case kCop0Cause:
        cause_ = (cause_ & ~kCauseSoftwareMask) | (low & kCauseSoftwareMask);
        UpdateInterruptPending();
        break;

    case kCop0Epc:
        epc_ = value;
        break;

    case kCop0Config:
        config_ = (config_ & ~kConfigWriteMask) | (low & kConfigWriteMask);
        break;

    case kCop0LlAddr:
        ll_addr_ = low;
        break;

    case kCop0WatchLo:
        watch_lo_ = low & 0xFFFFFFFB;
        break;

    case kCop0WatchHi:
        watch_hi_ = low & 0xF;
        break;

    case kCop0XContext:
        x_context_ = (x_context_ & 0x1FFFFFFF0ull) | (value & 0xFFFFFFFE00000000ull);
        break;

    case kCop0ParityError:
        parity_error_ = low & 0xFF;
        break;

    case kCop0TagLo:
        tag_lo_ = low;
        break;

    case kCop0ErrorEpc:
        error_epc_ = value;
        break;

    default:
        // Random, BadVAddr, PRId, CacheError, TagHi and the registers that don't exist ignore writes.
        break;
    }
}

void Cpu::WriteStatus(uint32_t value)
{
    status_ = value & kStatusWriteMask;
    translation_epoch_++;
    UpdateInterruptPending();
}

void Cpu::WriteCount(uint32_t value)
{
    count_offset_ = (uint64_t{value} << 1) - now_;
    ScheduleCompare();
    system_.OnCountWritten();
}

void Cpu::MoveToCop0(int index, uint64_t value)
{
    if (!late_events_)
    {
        WriteCop0(index, value);
        return;
    }

    // The check points of the legacy timing.
    if (index == kCop0Count)
    {
        // The check comes before the MTC0 is counted, and before the new value takes effect.
        const uint64_t ticks = system_.TicksPerInstruction();
        now_ -= ticks;
        system_.LateCheck(now_);
        now_ += ticks;
        WriteCop0(index, value);
        return;
    }

    WriteCop0(index, value);
    if (index == kCop0Status)
    {
        system_.RecheckInterrupts();
        status_check_ = true;
    }
}

void Cpu::WriteCompare(uint32_t value)
{
    compare_ = value;
    cause_ &= ~kCauseIp7;
    UpdateInterruptPending();
    ScheduleCompare();
}

void Cpu::ScheduleCompare()
{
    // The tick at which Count next becomes equal to Compare. When they're equal now, that's a full turn of Count away.
    const uint64_t full = now_ + count_offset_;
    uint64_t counts = (compare_ - static_cast<uint32_t>(full >> 1)) & 0xFFFFFFFF;
    if (counts == 0)
    {
        counts = uint64_t{1} << 32;
    }

    system_.Schedule(System::Event::kCompare, now_ + 2 * counts - (full & 1));
}

void Cpu::CompareReached(bool raise)
{
    if (raise)
    {
        cause_ |= kCauseIp7;
        UpdateInterruptPending();
    }

    ScheduleCompare();
}

void Cpu::MarkInterruptLineStale()
{
    ip2_stale_ = true;
    UpdateInterruptPending();
}

void Cpu::NoteCountUpdated()
{
    count_updated_ = now_;
}

void Cpu::SetInterruptLine(uint32_t cause_bit, bool raised)
{
    if (cause_bit == kCauseIp2)
    {
        ip2_stale_ = false;
    }

    if (raised)
    {
        cause_ |= cause_bit;
    }
    else
    {
        cause_ &= ~cause_bit;
    }

    UpdateInterruptPending();
}

void Cpu::UpdateInterruptPending()
{
    const uint32_t lines = ip2_stale_ ? cause_ & ~kCauseIp2 : cause_;
    interrupt_pending_ =
        (lines & status_ & 0xFF00) != 0 && (status_ & kStatusIe) != 0 && (status_ & (kStatusExl | kStatusErl)) == 0;
    if (interrupt_pending_)
    {
        system_.BreakSlice();
    }
}

void Cpu::TakeInterrupt()
{
    current_pc_ = pc_;
    in_delay_slot_ = branch_pending_;
    RaiseException(ExceptionCode::kInterrupt);
}

void Cpu::InvalidateFetchCache()
{
    translation_epoch_++;
}

void Cpu::SetExceptionHook(std::function<void(ExceptionCode, uint64_t)> hook)
{
    exception_hook_ = std::move(hook);
}

void Cpu::RaiseException(ExceptionCode code, uint32_t coprocessor)
{
    if (exception_hook_)
    {
        exception_hook_(code, current_pc_);
    }

    uint64_t vector = 0x180;
    if ((status_ & kStatusExl) == 0)
    {
        epc_ = in_delay_slot_ ? current_pc_ - 4 : current_pc_;
        if (in_delay_slot_)
        {
            cause_ |= kCauseBd;
        }
        else
        {
            cause_ &= ~kCauseBd;
        }

        status_ |= kStatusExl;
    }

    cause_ = (cause_ & ~(kCauseExceptionMask | kCauseCoprocessorMask)) | (static_cast<uint32_t>(code) << 2) |
             (coprocessor << 28);
    const uint64_t base = (status_ & kStatusBev) != 0 ? kVectorBaseBoot : kVectorBase;
    pc_ = base + vector;
    next_pc_ = pc_ + 4;
    branch_pending_ = false;
    translation_epoch_++;
    UpdateInterruptPending();
}

void Cpu::SetAddressFields(uint64_t vaddr)
{
    bad_vaddr_ = vaddr;
    context_ = (context_ & 0xFFFFFFFFFF800000ull) | (((vaddr >> 13) & 0x7FFFF) << 4);
    x_context_ =
        (x_context_ & 0xFFFFFFFE00000000ull) | (((vaddr >> 62) & 3) << 31) | (((vaddr >> 13) & 0x7FFFFFF) << 4);
    entry_hi_ = (entry_hi_ & 0xFF) | (vaddr & kVpn2Mask) | (vaddr & 0xC000000000000000ull);
    translation_epoch_++;
}

void Cpu::RaiseAddressError(uint64_t vaddr, Access access)
{
    SetAddressFields(vaddr);
    RaiseException(access == Access::kStore ? ExceptionCode::kAddressErrorStore : ExceptionCode::kAddressErrorLoad);
}

void Cpu::RaisePageFault(uint64_t vaddr, Access access, bool covered)
{
    // The legacy players raise this one exception for a fetch or a load. It replaces all of Cause, IP bits included,
    // and EntryHi's ASID. A later re-check of the interrupt lines sets IP2 again.
    if (exception_hook_)
    {
        exception_hook_(ExceptionCode::kTlbLoad, current_pc_);
    }

    bad_vaddr_ = vaddr;
    context_ = (context_ & 0xFFFFFFFFFF800000ull) | (((vaddr >> 13) & 0x7FFFF) << 4);
    entry_hi_ = (vaddr & kVpn2Mask) | (vaddr & 0xC000000000000000ull);

    uint64_t vector = 0x180;
    if ((status_ & kStatusExl) == 0)
    {
        epc_ = in_delay_slot_ ? current_pc_ - 4 : current_pc_;
        status_ |= kStatusExl;
        vector = covered ? 0x180 : 0;
    }
    else if (access == Access::kLoad)
    {
        // The legacy players still subtract 4 from EPC for a load, and 4 more in a delay slot.
        epc_ -= in_delay_slot_ ? 8 : 4;
    }

    cause_ = static_cast<uint32_t>(ExceptionCode::kTlbLoad) << 2;
    if (in_delay_slot_)
    {
        cause_ |= kCauseBd;
    }

    pc_ = kVectorBase + vector;
    next_pc_ = pc_ + 4;
    branch_pending_ = false;
    translation_epoch_++;
    UpdateInterruptPending();
}

void Cpu::RaiseTlbException(uint64_t vaddr, Access access, bool refill, bool modification)
{
    // A refill outside an exception uses the TLB refill vector, or the XTLB vector in a 64-bit mode.
    bool extended = false;
    const bool exception_level = (status_ & kStatusExl) != 0;
    if (refill && !exception_level)
    {
        extended = Decode(vaddr).extended;
    }

    SetAddressFields(vaddr);
    ExceptionCode code = ExceptionCode::kTlbLoad;
    if (modification)
    {
        code = ExceptionCode::kTlbModification;
    }
    else if (access == Access::kStore)
    {
        code = ExceptionCode::kTlbStore;
    }

    RaiseException(code);

    if (refill && !exception_level)
    {
        pc_ = pc_ - 0x180 + (extended ? 0x080 : 0x000);
        next_pc_ = pc_ + 4;
    }
}

Cpu::Segment Cpu::Decode(uint64_t vaddr) const
{
    const uint32_t ksu = (status_ >> 3) & 3;
    const bool kernel = (status_ & (kStatusExl | kStatusErl)) != 0 || ksu == 0;
    const bool supervisor = !kernel && ksu == 1;
    bool extended = false;
    if (kernel)
    {
        extended = (status_ & kStatusKx) != 0;
    }
    else if (supervisor)
    {
        extended = (status_ & kStatusSx) != 0;
    }
    else
    {
        extended = (status_ & kStatusUx) != 0;
    }

    if (!extended)
    {
        // In a 32-bit mode, an address must be a sign-extended 32-bit value.
        if (static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(vaddr))) != vaddr)
        {
            return {Segment::kError, 0, false};
        }

        const uint32_t address = static_cast<uint32_t>(vaddr);
        if (address < 0x80000000u)
        {
            return {Segment::kTlb, 0, false};
        }
        if (kernel)
        {
            if (address < 0xC0000000u)
            {
                return {Segment::kDirect, address & 0x1FFFFFFF, false};
            }

            return {Segment::kTlb, 0, false};
        }
        if (supervisor && address >= 0xC0000000u && address < 0xE0000000u)
        {
            return {Segment::kTlb, 0, false};
        }

        return {Segment::kError, 0, false};
    }

    // The 64-bit modes.
    if (vaddr <= 0x000000FFFFFFFFFFull)
    {
        return {Segment::kTlb, 0, true};
    }
    if (!kernel && !supervisor)
    {
        return {Segment::kError, 0, true};
    }
    if (vaddr >= 0x4000000000000000ull && vaddr <= 0x400000FFFFFFFFFFull)
    {
        return {Segment::kTlb, 0, true};
    }
    if (vaddr >= 0xFFFFFFFFC0000000ull && vaddr <= 0xFFFFFFFFDFFFFFFFull)
    {
        return {Segment::kTlb, 0, true};
    }
    if (supervisor)
    {
        return {Segment::kError, 0, true};
    }

    // Kernel only: xkphys, xkseg, ckseg0, ckseg1 and ckseg3.
    if (vaddr >= 0x8000000000000000ull && vaddr <= 0xBFFFFFFFFFFFFFFFull)
    {
        // xkphys is unmapped. Only its first 4 GiB in each cache mode exist.
        if ((vaddr & 0x07FFFFFF00000000ull) == 0)
        {
            return {Segment::kDirect, static_cast<uint32_t>(vaddr), true};
        }

        return {Segment::kError, 0, true};
    }
    if (vaddr >= 0xC000000000000000ull && vaddr <= 0xC00000FF7FFFFFFFull)
    {
        return {Segment::kTlb, 0, true};
    }
    if (vaddr >= 0xFFFFFFFF80000000ull && vaddr <= 0xFFFFFFFFBFFFFFFFull)
    {
        return {Segment::kDirect, static_cast<uint32_t>(vaddr) & 0x1FFFFFFF, true};
    }
    if (vaddr >= 0xFFFFFFFFE0000000ull)
    {
        return {Segment::kTlb, 0, true};
    }

    return {Segment::kError, 0, true};
}

void Cpu::PageTable::Set(uint32_t page, uint32_t value)
{
    // A page mask with a hole in it can give a range that runs past the top of memory.
    if (page >= kLeafSize * kLeafSize)
    {
        return;
    }

    std::unique_ptr<Leaf>& leaf = leaves_[page >> kLeafBits];
    if (!leaf)
    {
        if (value == 0)
        {
            return;
        }

        leaf = std::make_unique<Leaf>();
    }

    (*leaf)[page & (kLeafSize - 1)] = value;
}

void Cpu::PageTable::Clear()
{
    for (std::unique_ptr<Leaf>& leaf : leaves_)
    {
        leaf.reset();
    }
}

Cpu::TlbResult Cpu::LookupTlbPages(uint64_t vaddr, bool write, uint32_t& paddr) const
{
    const uint32_t page = static_cast<uint32_t>(vaddr) >> 12;
    const uint32_t value = write ? write_pages_.Get(page) : read_pages_.Get(page);
    if (value != 0)
    {
        paddr = (value & ~0xFFFu) | (static_cast<uint32_t>(vaddr) & 0xFFF);
        return TlbResult::kHit;
    }

    // No mapping. The scan takes in all 32 entries, and ignores their ASIDs, global bits and V bits. An unused entry of
    // zeros therefore covers 0 to 0x1FFF.
    for (const TlbEntry& entry : tlb_)
    {
        const uint64_t mask = kVpn2Mask & ~uint64_t{entry.page_mask};
        if ((vaddr & mask) == (entry.entry_hi & mask))
        {
            return TlbResult::kInvalid;
        }
    }

    return TlbResult::kMiss;
}

void Cpu::UnmapTlbPages(const TlbEntry& entry)
{
    const uint32_t half_pages = (((entry.page_mask | 0x1FFF) >> 1) + 1) >> 12;
    const uint32_t first_page = static_cast<uint32_t>(entry.entry_hi) >> 12;
    for (int half = 0; half < 2; half++)
    {
        const uint32_t entry_lo = half == 0 ? entry.entry_lo0 : entry.entry_lo1;
        if ((entry_lo & 2) == 0)
        {
            continue;
        }

        const uint32_t start = first_page + static_cast<uint32_t>(half) * half_pages;
        for (uint32_t n = 0; n < half_pages; n++)
        {
            read_pages_.Set(start + n, 0);
            if ((entry_lo & 4) != 0)
            {
                write_pages_.Set(start + n, 0);
            }
        }
    }
}

void Cpu::MapTlbPages(const TlbEntry& entry)
{
    const uint32_t half_pages = (((entry.page_mask | 0x1FFF) >> 1) + 1) >> 12;
    const uint32_t first_page = static_cast<uint32_t>(entry.entry_hi) >> 12;
    for (int half = 0; half < 2; half++)
    {
        // A half with V set sets the read mapping of its pages, and their write mapping only if D is set. The legacy
        // players refuse a half in kseg0 or kseg1, or one past the physical addresses.
        const uint32_t entry_lo = half == 0 ? entry.entry_lo0 : entry.entry_lo1;
        const uint32_t start = first_page + static_cast<uint32_t>(half) * half_pages;
        const uint32_t frame = static_cast<uint32_t>((uint64_t{entry_lo} >> 6) << 12);
        const bool direct = ((start << 12) & 0xC0000000u) == 0x80000000u;
        if ((entry_lo & 2) == 0 || direct || frame >= 0x20000000u)
        {
            continue;
        }

        for (uint32_t n = 0; n < half_pages; n++)
        {
            const uint32_t value = (frame + (n << 12)) | 1;
            read_pages_.Set(start + n, value);
            if ((entry_lo & 4) != 0)
            {
                write_pages_.Set(start + n, value);
            }
        }
    }
}

Cpu::TlbResult Cpu::LookupTlb(uint64_t vaddr, bool write, uint32_t& paddr) const
{
    if (tlb_pages_)
    {
        return LookupTlbPages(vaddr, write, paddr);
    }

    const uint32_t asid = static_cast<uint32_t>(entry_hi_ & 0xFF);
    for (const TlbEntry& entry : tlb_)
    {
        const uint64_t mask = kVpn2Mask & ~uint64_t{entry.page_mask};
        if ((vaddr & mask) != (entry.entry_hi & mask))
        {
            continue;
        }
        if ((vaddr >> 62) != (entry.entry_hi >> 62))
        {
            continue;
        }
        if (!entry.global && (entry.entry_hi & 0xFF) != asid)
        {
            continue;
        }

        // The page's offset bits, and the bit above them that picks the even or the odd page.
        const uint64_t offset_mask = (uint64_t{entry.page_mask} | 0x1FFF) >> 1;
        const bool odd = (vaddr & (offset_mask + 1)) != 0;
        const uint32_t entry_lo = odd ? entry.entry_lo1 : entry.entry_lo0;
        if ((entry_lo & 2) == 0)
        {
            return TlbResult::kInvalid;
        }
        if (write && (entry_lo & 4) == 0)
        {
            return TlbResult::kModified;
        }

        const uint64_t frame = (uint64_t{entry_lo} >> 6) << 12;
        paddr = static_cast<uint32_t>(frame + (vaddr & offset_mask));
        return TlbResult::kHit;
    }

    return TlbResult::kMiss;
}

bool Cpu::Translate(uint64_t vaddr, Access access, uint32_t& paddr)
{
    const Segment segment = Decode(vaddr);
    switch (segment.kind)
    {
    case Segment::kDirect:
        paddr = segment.paddr;
        return true;
    case Segment::kError:
        RaiseAddressError(vaddr, access);
        return false;
    case Segment::kTlb:
        break;
    }

    const TlbResult result = LookupTlb(vaddr, access == Access::kStore, paddr);
    if (result != TlbResult::kHit && tlb_pages_ && access == Access::kStore)
    {
        // The legacy players drop a store to a page with no writable mapping, and raise nothing.
        return false;
    }

    if (result != TlbResult::kHit && tlb_pages_)
    {
        RaisePageFault(vaddr, access, result == TlbResult::kInvalid);
        return false;
    }

    switch (result)
    {
    case TlbResult::kHit:
        return true;
    case TlbResult::kMiss:
        RaiseTlbException(vaddr, access, true, false);
        return false;
    case TlbResult::kInvalid:
        RaiseTlbException(vaddr, access, false, false);
        return false;
    case TlbResult::kModified:
        RaiseTlbException(vaddr, access, false, true);
        return false;
    }

    return false;
}

std::optional<uint32_t> Cpu::TranslateQuiet(uint64_t vaddr, bool write) const
{
    const Segment segment = Decode(vaddr);
    if (segment.kind == Segment::kDirect)
    {
        return segment.paddr;
    }
    if (segment.kind == Segment::kError)
    {
        return std::nullopt;
    }

    uint32_t paddr = 0;
    if (LookupTlb(vaddr, write, paddr) != TlbResult::kHit)
    {
        return std::nullopt;
    }

    return paddr;
}

const TlbEntry& Cpu::Tlb(int index) const
{
    return tlb_[index & 31];
}

void Cpu::SetTlb(int index, const TlbEntry& entry)
{
    // Write the entry through TLBWI's normalization, with the COP0 registers restored afterwards.
    const uint32_t page_mask = page_mask_;
    const uint64_t entry_hi = entry_hi_;
    const uint32_t entry_lo0 = entry_lo0_;
    const uint32_t entry_lo1 = entry_lo1_;

    page_mask_ = entry.page_mask & kPageMaskMask;
    entry_hi_ = entry.entry_hi & kEntryHiMask;
    entry_lo0_ = entry.entry_lo0 & kEntryLoMask;
    entry_lo1_ = entry.entry_lo1 & kEntryLoMask;
    if (entry.global)
    {
        entry_lo0_ |= 1;
        entry_lo1_ |= 1;
    }

    TlbWrite(index & 31);
    page_mask_ = page_mask;
    entry_hi_ = entry_hi;
    entry_lo0_ = entry_lo0;
    entry_lo1_ = entry_lo1;
}

void Cpu::TlbWrite(int index)
{
    if (index > 31)
    {
        return;
    }

    // The page mask takes effect in pairs of bits. The VPN2 bits under it and the frame bits above 32 bits of physical
    // address aren't kept. An entry is global only when both halves are.
    TlbEntry& entry = tlb_[index];
    if (tlb_pages_)
    {
        UnmapTlbPages(entry);
    }

    uint32_t page_mask = page_mask_ & 0x01554000;
    page_mask |= page_mask >> 1;
    entry.page_mask = page_mask;
    entry.global = (entry_lo0_ & entry_lo1_ & 1) != 0;
    const uint32_t global_bit = entry.global ? 1 : 0;
    entry.entry_lo0 = (entry_lo0_ & 0x03FFFFFE) | global_bit;
    entry.entry_lo1 = (entry_lo1_ & 0x03FFFFFE) | global_bit;
    entry.entry_hi = entry_hi_ & (kEntryHiMask & ~uint64_t{page_mask});
    translation_epoch_++;

    if (tlb_pages_)
    {
        MapTlbPages(entry);
    }
}

void Cpu::TlbRead()
{
    const uint32_t index = index_ & 0x3F;
    if (index > 31)
    {
        return;
    }

    const TlbEntry& entry = tlb_[index];
    page_mask_ = entry.page_mask;
    entry_hi_ = entry.entry_hi;
    entry_lo0_ = entry.entry_lo0;
    entry_lo1_ = entry.entry_lo1;
    translation_epoch_++;
}

void Cpu::TlbProbe()
{
    index_ = 0x80000000;
    const uint32_t asid = static_cast<uint32_t>(entry_hi_ & 0xFF);
    for (uint32_t i = 0; i < 32; i++)
    {
        const TlbEntry& entry = tlb_[i];
        const uint64_t mask = kVpn2Mask & ~uint64_t{entry.page_mask};
        if ((entry.entry_hi & mask) != (entry_hi_ & mask))
        {
            continue;
        }
        if ((entry.entry_hi >> 62) != (entry_hi_ >> 62))
        {
            continue;
        }
        if (!entry.global && (entry.entry_hi & 0xFF) != asid)
        {
            continue;
        }

        index_ = i;
        return;
    }
}

void Cpu::Eret()
{
    if ((status_ & kStatusErl) != 0)
    {
        pc_ = error_epc_;
        status_ &= ~kStatusErl;
    }
    else
    {
        pc_ = epc_;
        status_ &= ~kStatusExl;
    }

    next_pc_ = pc_ + 4;
    branch_pending_ = false;
    ll_bit_ = false;
    translation_epoch_++;
    UpdateInterruptPending();

    if (late_events_)
    {
        // The legacy timing never counts ERET. ERET re-checks the interrupt lines and then checks for a due event.
        now_ -= system_.TicksPerInstruction();
        system_.RecheckInterrupts();
        event_check_ = true;
    }
}

void Cpu::ExecuteCop0(uint32_t instruction)
{
    const bool kernel = (status_ & (kStatusExl | kStatusErl)) != 0 || ((status_ >> 3) & 3) == 0;
    const bool usable = kernel || (status_ & kStatusCu0) != 0;
    const uint32_t rs = Rs(instruction);
    const uint32_t rt = Rt(instruction);
    const int rd = static_cast<int>(Rd(instruction));

    if (rs < 0x10)
    {
        switch (rs)
        {
        case 0x00:
            if (!usable)
            {
                RaiseException(ExceptionCode::kCoprocessorUnusable, 0);
                return;
            }

            gpr_[rt] = SignExtend32(ReadCop0(rd));
            return;

        case 0x01:
            if (!usable)
            {
                RaiseException(ExceptionCode::kCoprocessorUnusable, 0);
                return;
            }
            if (!Allows64Bit())
            {
                RaiseException(ExceptionCode::kReservedInstruction);
                return;
            }

            gpr_[rt] = ReadCop0(rd);
            return;

        case 0x04:
            if (!usable)
            {
                RaiseException(ExceptionCode::kCoprocessorUnusable, 0);
                return;
            }

            MoveToCop0(rd, gpr_[rt]);
            return;

        case 0x05:
            if (!usable)
            {
                RaiseException(ExceptionCode::kCoprocessorUnusable, 0);
                return;
            }
            if (!Allows64Bit())
            {
                RaiseException(ExceptionCode::kReservedInstruction);
                return;
            }

            MoveToCop0(rd, gpr_[rt]);
            return;

        case 0x02:
        case 0x06:
        case 0x08:
            // CFC0, CTC0 and BC0 do nothing.
            return;

        default:
            RaiseException(ExceptionCode::kReservedInstruction);
            return;
        }
    }

    // The CO group. Functions that don't exist do nothing.
    const uint32_t funct = instruction & 0x3F;
    if (funct == 0x20 && system_.GetOptions().is_viewer)
    {
        // Test programs probe for an emulator's logging extensions with this function. Answering that there are none
        // sends their text to the debug port.
        gpr_[(instruction >> 20) & 31] = 0;
        return;
    }
    if (funct == 0x10)
    {
        RaiseException(ExceptionCode::kReservedInstruction);
        return;
    }
    if (funct != 0x01 && funct != 0x02 && funct != 0x06 && funct != 0x08 && funct != 0x18)
    {
        return;
    }
    if (!usable)
    {
        RaiseException(ExceptionCode::kCoprocessorUnusable, 0);
        return;
    }

    switch (funct)
    {
    case 0x01:
        TlbRead();
        break;
    case 0x02:
        TlbWrite(static_cast<int>(index_ & 0x3F));
        break;
    case 0x06:
        TlbWrite(static_cast<int>(Random()));
        break;
    case 0x08:
        TlbProbe();
        break;
    default:
        Eret();
        break;
    }
}

CpuState Cpu::State() const
{
    CpuState state;
    state.gpr = gpr_;
    state.hi = hi_;
    state.lo = lo_;
    state.pc = pc_;
    state.fpr = fpr_;
    state.fcr0 = 0x00000A00;
    state.fcr31 = fcr31_;
    for (int i = 0; i < 32; i++)
    {
        state.cop0[i] = ReadCop0(i);
    }
    state.ll_bit = ll_bit_;

    return state;
}

void Cpu::SetState(const CpuState& state)
{
    gpr_ = state.gpr;
    gpr_[0] = 0;
    hi_ = state.hi;
    lo_ = state.lo;
    fpr_ = state.fpr;
    fcr31_ = state.fcr31 & 0x0183FFFF;

    // Status first: it picks the mode that the other registers are read in.
    WriteStatus(static_cast<uint32_t>(state.cop0[kCop0Status]));
    wired_ = static_cast<uint32_t>(state.cop0[kCop0Wired]) & 0x3F;
    for (int i = 0; i < 32; i++)
    {
        if (i != kCop0Status && i != kCop0Wired && i != kCop0Compare && i != kCop0Count)
        {
            SetCop0(i, state.cop0[i]);
        }
    }

    compare_ = static_cast<uint32_t>(state.cop0[kCop0Compare]);
    WriteCount(static_cast<uint32_t>(state.cop0[kCop0Count]));
    latch_ = state.cop0[7];
    ll_bit_ = state.ll_bit;
    SetPc(state.pc);
    in_delay_slot_ = false;
    fetch_page_ = ~uint64_t{0};
    translation_epoch_++;
    UpdateInterruptPending();
}

} // namespace usf::n64
