// SPDX-License-Identifier: MIT

// The VR4300's COP0: its registers, the TLB, exceptions and interrupts, and address translation.

#include "n64/cpu_cop0.h"

#include <utility>

#include "n64/bits.h"
#include "n64/constants.h"
#include "n64/cpu.h"
#include "n64/system.h"

namespace usf::n64
{
namespace
{

// Whether `entry` covers the VPN2 of `vaddr`, under its page mask.
bool MatchesVpn2(const TlbEntry& entry, uint64_t vaddr)
{
    const uint64_t mask = kVpn2Mask & ~uint64_t{entry.page_mask};
    return (vaddr & mask) == (entry.entry_hi & mask);
}

// Whether `entry` maps `vaddr` for `asid`. The VPN2 and the region bits must match, and so must the ASID unless the
// entry is global.
bool Matches(const TlbEntry& entry, uint64_t vaddr, uint32_t asid)
{
    return MatchesVpn2(entry, vaddr) && (vaddr >> kRegionShift) == (entry.entry_hi >> kRegionShift) &&
           (entry.global || (entry.entry_hi & kEntryHiAsidMask) == asid);
}

// The size of each half of an entry: one page of the page mask's size.
uint32_t HalfSize(const TlbEntry& entry)
{
    return ((entry.page_mask | kMinPairMask) >> 1) + 1;
}

// The physical address of the page that an EntryLo maps.
uint64_t FrameOf(uint32_t entry_lo)
{
    return (uint64_t{entry_lo} >> kEntryLoPfnShift) << kPageBits;
}

// Context's BadVPN2 for `vaddr`: bits 13 to 31 of the address, at bit 4.
uint64_t ContextBadVpn2(uint64_t vaddr)
{
    return ((vaddr >> 13) & 0x7FFFF) << 4;
}

// XContext's R and BadVPN2 for `vaddr`: the address's top two bits, and its bits 13 to 39 at bit 4.
uint64_t XContextBadVpn2(uint64_t vaddr)
{
    return (((vaddr >> kRegionShift) & 3) << kXContextRegionShift) | (((vaddr >> 13) & 0x7FFFFFF) << 4);
}

// EntryHi's R and VPN2 for `vaddr`.
uint64_t EntryHiVpn2(uint64_t vaddr)
{
    return (vaddr & kVpn2Mask) | (vaddr & kEntryHiRegionMask);
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
        context_ = value & (kContextPteBaseMask | kContextBadVpn2Mask);
        break;

    case kCop0XContext:
        x_context_ = value & (kXContextPteBaseMask | kXContextBadVpn2Mask);
        break;

    case kCop0Cause:
        cause_ = static_cast<uint32_t>(value) & kCauseDefinedMask;
        UpdateInterruptPending();
        break;

    case kCop0Wired:
        wired_ = static_cast<uint32_t>(value) & kWiredMask;
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
        context_ = (context_ & kContextBadVpn2Mask) | (value & kContextPteBaseMask);
        break;

    case kCop0PageMask:
        page_mask_ = low & kPageMaskMask;
        break;

    case kCop0Wired:
        wired_ = low & kWiredMask;
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
        watch_lo_ = low & kWatchLoMask;
        break;

    case kCop0WatchHi:
        watch_hi_ = low & kWatchHiMask;
        break;

    case kCop0XContext:
        x_context_ = (x_context_ & kXContextBadVpn2Mask) | (value & kXContextPteBaseMask);
        break;

    case kCop0ParityError:
        parity_error_ = low & kParityErrorMask;
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
    uint64_t counts = compare_ - static_cast<uint32_t>(full >> 1);
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

void Cpu::SetIp2(bool raised)
{
    ip2_stale_ = false;
    if (raised)
    {
        cause_ |= kCauseIp2;
    }
    else
    {
        cause_ &= ~kCauseIp2;
    }

    UpdateInterruptPending();
}

void Cpu::UpdateInterruptPending()
{
    const uint32_t lines = ip2_stale_ ? cause_ & ~kCauseIp2 : cause_;
    interrupt_pending_ = (lines & status_ & kInterruptMask) != 0 && (status_ & kStatusIe) != 0 &&
                         (status_ & (kStatusExl | kStatusErl)) == 0;
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

void Cpu::RaiseException(ExceptionCode code, uint32_t coprocessor, uint64_t vector)
{
    if (exception_hook_)
    {
        exception_hook_(code, current_pc_);
    }

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

    cause_ = (cause_ & ~(kCauseExceptionMask | kCauseCoprocessorMask)) |
             (static_cast<uint32_t>(code) << kCauseExceptionShift) | (coprocessor << kCauseCoprocessorShift);
    const uint64_t base = (status_ & kStatusBev) != 0 ? kVectorBaseBoot : kVectorBase;
    EnterVector(base + vector);
}

void Cpu::EnterVector(uint64_t address)
{
    pc_ = address;
    next_pc_ = pc_ + 4;
    branch_pending_ = false;
    translation_epoch_++;
    UpdateInterruptPending();
}

void Cpu::SetAddressFields(uint64_t vaddr)
{
    bad_vaddr_ = vaddr;
    context_ = (context_ & kContextPteBaseMask) | ContextBadVpn2(vaddr);
    x_context_ = (x_context_ & kXContextPteBaseMask) | XContextBadVpn2(vaddr);
    entry_hi_ = (entry_hi_ & kEntryHiAsidMask) | EntryHiVpn2(vaddr);
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
    context_ = (context_ & kContextPteBaseMask) | ContextBadVpn2(vaddr);
    entry_hi_ = EntryHiVpn2(vaddr);

    uint64_t vector = kGeneralVector;
    if ((status_ & kStatusExl) == 0)
    {
        epc_ = in_delay_slot_ ? current_pc_ - 4 : current_pc_;
        status_ |= kStatusExl;
        vector = covered ? kGeneralVector : kTlbRefillVector;
    }
    else if (access == Access::kLoad)
    {
        // The legacy players still subtract 4 from EPC for a load, and 4 more in a delay slot.
        epc_ -= in_delay_slot_ ? 8 : 4;
    }

    cause_ = static_cast<uint32_t>(ExceptionCode::kTlbLoad) << kCauseExceptionShift;
    if (in_delay_slot_)
    {
        cause_ |= kCauseBd;
    }

    EnterVector(kVectorBase + vector);
}

void Cpu::RaiseTlbException(uint64_t vaddr, Access access, bool refill, bool modification)
{
    // A refill outside an exception uses the TLB refill vector, or the XTLB vector in a 64-bit mode.
    uint64_t vector = kGeneralVector;
    if (refill && (status_ & kStatusExl) == 0)
    {
        vector = Decode(vaddr).extended ? kXtlbRefillVector : kTlbRefillVector;
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

    RaiseException(code, 0, vector);
}

Cpu::Segment Cpu::Decode(uint64_t vaddr) const
{
    const bool kernel = KernelMode();
    const bool supervisor = !kernel && Ksu() == kKsuSupervisor;
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
        if (SignExtend32(vaddr) != vaddr)
        {
            return {Segment::kError, 0, false};
        }

        const uint32_t address = static_cast<uint32_t>(vaddr);
        if (address < kKseg0)
        {
            return {Segment::kTlb, 0, false};
        }
        if (kernel)
        {
            if (address < kKseg2)
            {
                return {Segment::kDirect, address & kPhysicalAddressMask, false};
            }

            return {Segment::kTlb, 0, false};
        }
        if (supervisor && address >= kKseg2 && address < kKseg3)
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
        return {Segment::kDirect, static_cast<uint32_t>(vaddr) & kPhysicalAddressMask, true};
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
    const uint32_t page = static_cast<uint32_t>(vaddr) >> kPageBits;
    const uint32_t value = write ? write_pages_.Get(page) : read_pages_.Get(page);
    if (value != 0)
    {
        paddr = (value & ~kPageOffsetMask) | (static_cast<uint32_t>(vaddr) & kPageOffsetMask);
        return TlbResult::kHit;
    }

    // No mapping. The scan takes in all 32 entries, and ignores their ASIDs, global bits and V bits. An unused entry of
    // zeros therefore covers 0 to 0x1FFF.
    for (const TlbEntry& entry : tlb_)
    {
        if (MatchesVpn2(entry, vaddr))
        {
            return TlbResult::kInvalid;
        }
    }

    return TlbResult::kMiss;
}

bool Cpu::MapsPastPhysical(uint64_t vaddr) const
{
    for (const TlbEntry& entry : tlb_)
    {
        if (!MatchesVpn2(entry, vaddr))
        {
            continue;
        }

        const uint32_t half_size = HalfSize(entry);
        const uint32_t entry_lo = (static_cast<uint32_t>(vaddr) & half_size) != 0 ? entry.entry_lo1 : entry.entry_lo0;
        const uint64_t frame = FrameOf(entry_lo);
        if ((entry_lo & kEntryLoValid) != 0 && frame >= kPhysicalAddressLimit)
        {
            return true;
        }
    }

    return false;
}

uint64_t Cpu::RefusedBoardPages() const
{
    return refused_board_pages_;
}

void Cpu::UnmapTlbPages(const TlbEntry& entry)
{
    const uint32_t half_pages = HalfSize(entry) >> kPageBits;
    const uint32_t first_page = static_cast<uint32_t>(entry.entry_hi) >> kPageBits;
    for (int half = 0; half < 2; half++)
    {
        const uint32_t entry_lo = half == 0 ? entry.entry_lo0 : entry.entry_lo1;
        if ((entry_lo & kEntryLoValid) == 0)
        {
            continue;
        }

        const uint32_t start = first_page + static_cast<uint32_t>(half) * half_pages;
        for (uint32_t n = 0; n < half_pages; n++)
        {
            read_pages_.Set(start + n, 0);
            if ((entry_lo & kEntryLoDirty) != 0)
            {
                write_pages_.Set(start + n, 0);
            }
        }
    }
}

void Cpu::MapTlbPages(const TlbEntry& entry)
{
    const uint32_t half_pages = HalfSize(entry) >> kPageBits;
    const uint32_t first_page = static_cast<uint32_t>(entry.entry_hi) >> kPageBits;

    // An arcade board has RAM above the console's physical addresses. Its games map pages there.
    const bool board = system_.GetOptions().board != Board::kConsole;
    for (int half = 0; half < 2; half++)
    {
        // A half with V set sets the read mapping of its pages, and their write mapping only if D is set. The legacy
        // players refuse a half in kseg0 or kseg1, or one past the physical addresses.
        const uint32_t entry_lo = half == 0 ? entry.entry_lo0 : entry.entry_lo1;
        const uint32_t start = first_page + static_cast<uint32_t>(half) * half_pages;
        const uint32_t frame = static_cast<uint32_t>(FrameOf(entry_lo));
        const bool direct = ((start << kPageBits) & kSegmentMask) == kKseg0;
        if ((entry_lo & kEntryLoValid) == 0 || direct || (frame >= kPhysicalAddressLimit && !board))
        {
            continue;
        }

        for (uint32_t n = 0; n < half_pages; n++)
        {
            const uint32_t value = (frame + (n << kPageBits)) | 1;
            read_pages_.Set(start + n, value);
            if ((entry_lo & kEntryLoDirty) != 0)
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

    const uint32_t asid = static_cast<uint32_t>(entry_hi_ & kEntryHiAsidMask);
    for (const TlbEntry& entry : tlb_)
    {
        if (!Matches(entry, vaddr, asid))
        {
            continue;
        }

        // The page's offset bits, and the bit above them that picks the even or the odd page.
        const uint64_t offset_mask = HalfSize(entry) - 1;
        const bool odd = (vaddr & (offset_mask + 1)) != 0;
        const uint32_t entry_lo = odd ? entry.entry_lo1 : entry.entry_lo0;
        if ((entry_lo & kEntryLoValid) == 0)
        {
            return TlbResult::kInvalid;
        }
        if (write && (entry_lo & kEntryLoDirty) == 0)
        {
            return TlbResult::kModified;
        }

        const uint64_t frame = FrameOf(entry_lo);
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
    if (result != TlbResult::kHit && tlb_pages_)
    {
        if (MapsPastPhysical(vaddr))
        {
            refused_board_pages_++;
        }
        if (access == Access::kStore)
        {
            // The legacy players drop a store to a page with no writable mapping, and raise nothing.
            return false;
        }

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
        entry_lo0_ |= kEntryLoGlobal;
        entry_lo1_ |= kEntryLoGlobal;
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

    uint32_t page_mask = page_mask_ & kPageMaskPairs;
    page_mask |= page_mask >> 1;
    entry.page_mask = page_mask;
    entry.global = (entry_lo0_ & entry_lo1_ & kEntryLoGlobal) != 0;
    const uint32_t global_bit = entry.global ? kEntryLoGlobal : 0;
    entry.entry_lo0 = (entry_lo0_ & kEntryLoKeptBits) | global_bit;
    entry.entry_lo1 = (entry_lo1_ & kEntryLoKeptBits) | global_bit;
    entry.entry_hi = entry_hi_ & (kEntryHiMask & ~uint64_t{page_mask});
    translation_epoch_++;

    if (tlb_pages_)
    {
        MapTlbPages(entry);
    }
}

void Cpu::TlbRead()
{
    const uint32_t index = index_ & kIndexEntryMask;
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
    index_ = kIndexProbeFailure;
    const uint32_t asid = static_cast<uint32_t>(entry_hi_ & kEntryHiAsidMask);
    for (uint32_t i = 0; i < 32; i++)
    {
        if (Matches(tlb_[i], entry_hi_, asid))
        {
            index_ = i;
            return;
        }
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
    const bool usable = KernelMode() || (status_ & kStatusCu0) != 0;
    const uint32_t rs = Rs(instruction);
    const uint32_t rt = Rt(instruction);
    const int rd = static_cast<int>(Rd(instruction));

    if (rs < 0x10)
    {
        switch (rs)
        {
        case 0x00:
        case 0x01:
        case 0x04:
        case 0x05:
            {
                // MFC0, DMFC0, MTC0 and DMTC0. The doubleword moves are reserved in 32-bit user and supervisor modes.
                const bool doubleword = (rs & 1) != 0;
                if (!usable)
                {
                    RaiseException(ExceptionCode::kCoprocessorUnusable, 0);
                    return;
                }
                if (doubleword && !Allows64Bit())
                {
                    RaiseException(ExceptionCode::kReservedInstruction);
                    return;
                }

                if (rs >= 0x04)
                {
                    MoveToCop0(rd, gpr_[rt]);
                }
                else
                {
                    gpr_[rt] = doubleword ? ReadCop0(rd) : SignExtend32(ReadCop0(rd));
                }
                return;
            }

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
        // sends their text to the debug port. The probe names its destination register in bits 20 to 24, not in rt.
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
        TlbWrite(static_cast<int>(index_ & kIndexEntryMask));
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
    state.fcr0 = kFcr0;
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
    SetFcr31(state.fcr31);

    // Status first: it picks the mode that the other registers are read in.
    WriteStatus(static_cast<uint32_t>(state.cop0[kCop0Status]));
    wired_ = static_cast<uint32_t>(state.cop0[kCop0Wired]) & kWiredMask;
    for (int i = 0; i < 32; i++)
    {
        if (i != kCop0Status && i != kCop0Wired && i != kCop0Compare && i != kCop0Count)
        {
            SetCop0(i, state.cop0[i]);
        }
    }

    compare_ = static_cast<uint32_t>(state.cop0[kCop0Compare]);
    WriteCount(static_cast<uint32_t>(state.cop0[kCop0Count]));

    // Register 7 doesn't exist. A read of it gives the latch. The state keeps the latch there.
    latch_ = state.cop0[7];
    ll_bit_ = state.ll_bit;
    SetPc(state.pc);
    in_delay_slot_ = false;
    fetch_page_ = ~uint64_t{0};
    translation_epoch_++;
    UpdateInterruptPending();
}

} // namespace usf::n64
