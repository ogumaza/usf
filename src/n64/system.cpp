// SPDX-License-Identifier: MIT

// The system's clock, its events, its buses and its API. The devices are in rcp.cpp, boot in boot.cpp and save
// states in save_state.cpp.

#include "n64/system.h"

#include <algorithm>
#include <cmath>

#include "xsflib/bytes.h"

namespace usf::n64
{

namespace
{

// While the RSP runs, the CPU runs in slices of this many ticks, and the RSP catches up after each one.
constexpr uint64_t kRspSliceTicks = 96;

// How long a write to cartridge space stays in the PI's latch, with the hardware's timing.
constexpr uint64_t kCartWriteLatchTicks = 200;

// The header's country codes for PAL games.
bool IsPalCountry(uint8_t code)
{
    switch (code)
    {
    case 0x44:
    case 0x46:
    case 0x49:
    case 0x50:
    case 0x53:
    case 0x55:
    case 0x58:
    case 0x59:
        return true;
    default:
        return false;
    }
}

} // namespace

bool QuietReport::Quiet() const
{
    return !cpu_in_delay_slot && !rsp_running && !sp_interrupt_pending && !sp_dma_busy && !pi_dma_busy &&
           !si_dma_busy && !dp_busy && !ai_buffer_waiting;
}

std::string QuietReport::Describe() const
{
    std::string text;
    auto add = [&text](const char* part)
    {
        if (!text.empty())
        {
            text += ", ";
        }
        text += part;
    };

    if (cpu_in_delay_slot)
    {
        add("the CPU is in a delay slot");
    }
    if (rsp_running)
    {
        add("the RSP is running");
    }
    if (sp_interrupt_pending)
    {
        add("an RSP task's interrupt is still to come");
    }
    if (sp_dma_busy)
    {
        add("an SP DMA is in flight");
    }
    if (pi_dma_busy)
    {
        add("a PI DMA is in flight");
    }
    if (si_dma_busy)
    {
        add("an SI DMA is in flight");
    }
    if (dp_busy)
    {
        add("the RDP is busy");
    }
    if (ai_buffer_waiting)
    {
        add("a second audio buffer waits");
    }

    return text.empty() ? "quiet" : text;
}

System::System(const Options& options) : options_(options)
{
    cpu_ = std::make_unique<Cpu>(*this);
    rsp_ = std::make_unique<Rsp>(*this);
    Reset();
}

System::~System() = default;

void System::Reset()
{
    now_ = 0;
    events_.fill(kNever);
    event_order_.fill(0);
    event_serial_ = 0;
    next_event_ = kNever;
    interrupt_event_ = false;
    interrupt_event_time_ = 0;
    in_event_handler_ = false;
    stop_ = false;
    slice_break_ = false;
    hit_breakpoint_ = false;
    resume_at_breakpoint_ = false;

    rdram_.assign(options_.rdram_size, 0);
    is_viewer_.assign(options_.is_viewer ? kIsViewerSize : 0, 0);
    mi_ = {};
    vi_ = {};
    ai_ = {};
    pi_ = {};
    si_ = {};
    dp_ = {};
    ri_ = {};
    rdram_noise_ = 0x2545F491;
    direct_rdram_size_ = 0;
    pif_.Reset();
    rsp_->Reset();
    cpu_->Reset();
    cpu_->InvalidateFetchCache();

    // Until a boot puts them back to their power-on state for IPL3, the chips are ready as plain memory, so that a
    // program can run from RDRAM without IPL3.
    ResetRdramChips(true);

    cpu_->SetCop0(kCop0Compare, 0);
    rsp_credit_ = 0;
    rsp_clock_ = 0;
    task_type_ = 0;
    task_start_ = 0;
    graphics_task_pending_ = false;
    graphics_dp_pending_ = false;
    sp_interrupt_deferred_ = false;

    if (coverage_enabled_)
    {
        coverage_ = std::make_unique<Coverage>(options_.rdram_size);
    }

    tv_ = TvType::kNtsc;
    if (rom_.size() > 0x3E && IsPalCountry(rom_[0x3E]))
    {
        tv_ = TvType::kPal;
    }

    StartVi();
}

void System::LoadRom(std::vector<uint8_t> rom)
{
    // The first word of a cartridge's header is 0x80371240 in console order. The two other orders swap the bytes of
    // each halfword or reverse the bytes of each word.
    rom.resize((rom.size() + 3) & ~size_t{3}, 0);
    if (rom.size() >= 4 && rom[0] == 0x37 && rom[1] == 0x80)
    {
        for (size_t i = 0; i + 1 < rom.size(); i += 2)
        {
            std::swap(rom[i], rom[i + 1]);
        }
    }
    else if (rom.size() >= 4 && rom[0] == 0x40 && rom[1] == 0x12)
    {
        for (size_t i = 0; i + 3 < rom.size(); i += 4)
        {
            std::swap(rom[i], rom[i + 3]);
            std::swap(rom[i + 1], rom[i + 2]);
        }
    }

    LoadRomImage(std::move(rom));
}

void System::LoadRomImage(std::vector<uint8_t> rom)
{
    rom_ = std::move(rom);
    rom_image_size_ = 0x100000;
    while (rom_image_size_ < rom_.size() && rom_image_size_ < 0x10000000)
    {
        rom_image_size_ *= 2;
    }

    Reset();
}

const std::vector<uint8_t>& System::Rom() const
{
    return rom_;
}

void System::SetPifRom(std::vector<uint8_t> pif_rom)
{
    pif_rom_ = std::move(pif_rom);
}

void System::PatchRom(uint32_t offset, std::span<const uint8_t> bytes)
{
    const size_t end = size_t{offset} + bytes.size();
    if (end > rom_.size())
    {
        rom_.resize(end, 0);
    }

    std::copy(bytes.begin(), bytes.end(), rom_.begin() + offset);

    while (rom_image_size_ < rom_.size() && rom_image_size_ < 0x10000000)
    {
        rom_image_size_ = std::max<uint32_t>(rom_image_size_ * 2, 0x100000);
    }
}

// Time and events.

uint64_t& System::Clock()
{
    return now_;
}

uint64_t System::Now() const
{
    return now_;
}

uint64_t System::Fields() const
{
    return vi_.fields;
}

uint64_t System::TicksPerInstruction() const
{
    return options_.timing == Timing::kLegacy ? 4 : 1;
}

uint64_t System::TicksPerSecond() const
{
    if (options_.timing == Timing::kLegacy)
    {
        // Count units run at about the video clock, and a count unit is two ticks.
        return 2 * VideoClock();
    }

    return kCpuClock;
}

const Options& System::GetOptions() const
{
    return options_;
}

TvType System::Tv() const
{
    return tv_;
}

uint64_t System::NextEvent() const
{
    // An interrupt that waits in the queue is due at once.
    return interrupt_event_ ? 0 : next_event_;
}

void System::LateCheck(uint64_t due)
{
    // One check point of the legacy timing. An interrupt that waits goes first. Otherwise the first event that is due
    // by `due` is handled, with the clock as it stands.
    if (interrupt_event_)
    {
        interrupt_event_ = false;
        if (cpu_->InterruptPending())
        {
            cpu_->EnterInterrupt();
        }
        return;
    }
    if (next_event_ > due)
    {
        return;
    }

    int first = -1;
    for (int i = 0; i < static_cast<int>(Event::kCount); i++)
    {
        if (events_[i] == kNever)
        {
            continue;
        }

        if (first < 0 || events_[i] < events_[first] ||
            (events_[i] == events_[first] && event_order_[i] < event_order_[first]))
        {
            first = i;
        }
    }

    events_[first] = kNever;
    RecomputeNextEvent();

    // An interrupt that the handler raises, and that Status allows, is taken inside this check.
    in_event_handler_ = true;
    HandleEvent(static_cast<Event>(first));
    in_event_handler_ = false;
    if (cpu_->InterruptPending())
    {
        cpu_->EnterInterrupt();
    }
}

void System::RecheckInterrupts()
{
    if (!LateEvents())
    {
        return;
    }

    // Each re-check copies the AI's latch to MI_INTR and recomputes IP2.
    mi_.interrupt = (mi_.interrupt & ~kMiInterruptAi) | (ai_.interrupt_latch ? kMiInterruptAi : 0);
    UpdateCpuInterrupt();

    // A write that changes the interrupt lines queues an interrupt at the front when Status allows one. An event's
    // handler takes its interrupt in its check instead.
    if (!in_event_handler_ && cpu_->InterruptPending())
    {
        interrupt_event_ = true;
        interrupt_event_time_ = now_;
    }
}

uint64_t System::FirstEventTime() const
{
    return interrupt_event_ ? interrupt_event_time_ : next_event_;
}

void System::OnCountWritten()
{
    if (LateEvents())
    {
        ScheduleCountWrap();
    }
}

void System::ScheduleCountWrap()
{
    // The next time Count reaches a multiple of 0x80000000.
    const uint32_t count = cpu_->Count();
    uint64_t counts = 0x80000000u - (count & 0x7FFFFFFFu);
    Schedule(Event::kCountWrap, now_ + 2 * counts);
}

bool System::LateEvents() const
{
    return options_.timing == Timing::kLegacy && !options_.exact_events;
}

void System::RecomputeNextEvent()
{
    next_event_ = *std::min_element(events_.begin(), events_.end());
}

void System::Schedule(Event event, uint64_t when)
{
    events_[static_cast<int>(event)] = when;
    event_order_[static_cast<int>(event)] = ++event_serial_;
    RecomputeNextEvent();
    slice_break_ = true;
}

void System::Cancel(Event event)
{
    events_[static_cast<int>(event)] = kNever;
    RecomputeNextEvent();
}

void System::FireEvents()
{
    while (next_event_ <= now_ && !stop_)
    {
        int first = 0;
        for (int i = 1; i < static_cast<int>(Event::kCount); i++)
        {
            if (events_[i] < events_[first])
            {
                first = i;
            }
        }
        events_[first] = kNever;
        RecomputeNextEvent();
        HandleEvent(static_cast<Event>(first));
    }
}

void System::HandleEvent(Event event)
{
    switch (event)
    {
    case Event::kVi:
        FinishViField();
        break;
    case Event::kAiBuffer:
        FinishAiBuffer();
        break;
    case Event::kPi:
        FinishPiDma();
        break;
    case Event::kSi:
        FinishSiDma();
        break;
    case Event::kSpDma:
        rsp_->FinishDma();
        break;
    case Event::kCompare:
        cpu_->CompareReached(options_.timer_interrupt);
        break;
    case Event::kSpTask:
        FinishSkippedTask();
        break;
    case Event::kCountWrap:
        ScheduleCountWrap();
        break;
    case Event::kCount:
        break;
    }
}

void System::BreakSlice()
{
    slice_break_ = true;
}

bool& System::SliceBreakFlag()
{
    return slice_break_;
}

void System::Run(uint64_t ticks)
{
    stop_ = false;
    hit_breakpoint_ = false;
    const uint64_t end = now_ + ticks;
    if (LateEvents())
    {
        RunLate(end);
        return;
    }

    while (now_ < end && !stop_)
    {
        FireEvents();
        if (stop_)
        {
            break;
        }

        uint64_t slice_end = std::min(end, next_event_);
        if (!rsp_->Halted())
        {
            slice_end = std::min(slice_end, now_ + kRspSliceTicks);
        }

        slice_break_ = false;
        if (slice_end > now_)
        {
            cpu_->Run(slice_end);
        }

        CatchUpRsp();
    }
}

void System::RunLate(uint64_t end)
{
    // Events are handled only where the CPU checks for them. The legacy players run each RSP task to completion when it
    // starts. The RSP never needs a slice.
    while (now_ < end && !stop_)
    {
        slice_break_ = false;
        cpu_->Run(end);
        CatchUpRsp();
    }
}

void System::RunSeconds(double seconds)
{
    Run(static_cast<uint64_t>(std::llround(seconds * static_cast<double>(TicksPerSecond()))));
}

void System::Stop()
{
    stop_ = true;
    slice_break_ = true;
}

bool System::Stopped() const
{
    return stop_;
}

void System::CatchUpRsp()
{
    if (rsp_->Halted())
    {
        rsp_clock_ = now_;
        rsp_credit_ = 0;
        return;
    }
    if (now_ <= rsp_clock_)
    {
        return;
    }

    // The RSP runs two cycles for every three ticks of the CPU's clock.
    const uint64_t thirds = (now_ - rsp_clock_) * 2 + rsp_credit_;
    rsp_credit_ = thirds % 3;
    rsp_clock_ = now_;
    rsp_->Run(thirds / 3);
}

// Breakpoints.

void System::AddBreakpoint(uint64_t vaddr)
{
    if (std::find(breakpoints_.begin(), breakpoints_.end(), vaddr) == breakpoints_.end())
    {
        breakpoints_.push_back(vaddr);
    }
}

void System::RemoveBreakpoint(uint64_t vaddr)
{
    breakpoints_.erase(std::remove(breakpoints_.begin(), breakpoints_.end(), vaddr), breakpoints_.end());
}

void System::ClearBreakpoints()
{
    breakpoints_.clear();
}

bool System::HitBreakpoint() const
{
    return hit_breakpoint_;
}

bool System::HasBreakpoints() const
{
    return !breakpoints_.empty();
}

bool System::ShouldBreak(uint64_t pc)
{
    if (resume_at_breakpoint_)
    {
        // The run that follows a breakpoint executes the instruction there.
        resume_at_breakpoint_ = false;
        if (pc == break_pc_)
        {
            return false;
        }
    }
    if (std::find(breakpoints_.begin(), breakpoints_.end(), pc) == breakpoints_.end())
    {
        return false;
    }

    hit_breakpoint_ = true;
    resume_at_breakpoint_ = true;
    break_pc_ = pc;
    stop_ = true;
    slice_break_ = true;

    return true;
}

// Interrupts.

void System::RaiseMi(uint32_t bits)
{
    if ((bits & kMiInterruptAi) != 0)
    {
        ai_.interrupt_latch = true;
    }

    mi_.interrupt |= bits;
    UpdateCpuInterrupt();
    RecheckInterrupts();
}

void System::LowerMi(uint32_t bits)
{
    mi_.interrupt &= ~bits;
    UpdateCpuInterrupt();
    RecheckInterrupts();
}

void System::UpdateCpuInterrupt()
{
    cpu_->SetInterruptLine(kCauseIp2, (mi_.interrupt & mi_.mask & 0x3F) != 0);
}

// The RSP's tasks.

void System::OnSpStatusWritten(uint32_t old_status)
{
    const bool started = (old_status & kSpStatusHalt) != 0 && !rsp_->Halted();
    if (started)
    {
        StartRspTask();
    }
}

void System::StartRspTask()
{
    rsp_clock_ = now_;
    rsp_credit_ = 0;
    task_start_ = now_;
    task_type_ = xsflib::LoadBe32(rsp_->Dmem().data() + 0xFC0);
    sp_interrupt_deferred_ = false;

    if (options_.skip_graphics_tasks && task_type_ == 1)
    {
        // A graphics task doesn't run. The RSP stays halted, and the task completes 1,000 count units later. Without
        // the delays, the DP's interrupt comes at once.
        rsp_->SetStatusBits(kSpStatusHalt);
        graphics_task_pending_ = true;
        graphics_dp_pending_ = RcpDelays();
        if (!graphics_dp_pending_)
        {
            RaiseMi(kMiInterruptDp);
        }
        Schedule(Event::kSpTask, now_ + 2000);
        return;
    }

    if (options_.timing == Timing::kLegacy)
    {
        // The legacy players run a task to completion at once.
        uint64_t budget = 200'000'000;
        while (!rsp_->Halted() && budget > 0)
        {
            budget -= rsp_->Run(std::min<uint64_t>(budget, 1'000'000));
            if (rsp_->DmaBusy())
            {
                rsp_->FinishDma();
            }
        }
        return;
    }

    BreakSlice();
}

void System::OnRspBroke()
{
    if ((rsp_->Status() & kSpStatusInterruptOnBreak) == 0)
    {
        return;
    }

    if (options_.timing == Timing::kLegacy && task_type_ == 2)
    {
        // The legacy players raise an audio task's interrupt 4,000 count units after the task starts.
        sp_interrupt_deferred_ = true;
        Schedule(Event::kSpTask, std::max(now_, task_start_ + 8000));
        return;
    }

    RaiseMi(kMiInterruptSp);
}

void System::FinishSkippedTask()
{
    if (graphics_task_pending_)
    {
        graphics_task_pending_ = false;
        rsp_->SetStatusBits(kSpStatusHalt | kSpStatusBroke | (kSpStatusSignal0 << 2));
        if ((rsp_->Status() & kSpStatusInterruptOnBreak) != 0)
        {
            RaiseMi(kMiInterruptSp);
        }
        if (graphics_dp_pending_)
        {
            graphics_dp_pending_ = false;
            RaiseMi(kMiInterruptDp);
        }
    }

    if (sp_interrupt_deferred_)
    {
        sp_interrupt_deferred_ = false;
        RaiseMi(kMiInterruptSp);
    }
}

// Buses.

void System::DmaReadRdram(uint32_t address, uint8_t* out, uint32_t length)
{
    for (uint32_t i = 0; i < length; i++)
    {
        const uint32_t a = address + i;
        out[i] = a < rdram_.size() ? rdram_[a] : 0;
    }

    if (Coverage* coverage = ActiveCoverage())
    {
        coverage->ReadRdram(address, length);
    }
}

void System::DmaWriteRdram(uint32_t address, const uint8_t* data, uint32_t length)
{
    for (uint32_t i = 0; i < length; i++)
    {
        const uint32_t a = address + i;
        if (a < rdram_.size())
        {
            rdram_[a] = data[i];
        }
    }

    if (Coverage* coverage = ActiveCoverage())
    {
        coverage->WriteRdram(address, length);
    }
}

uint32_t System::ReadBus32(uint32_t paddr)
{
    paddr &= ~3u;
    if (paddr < kRdramRegisters)
    {
        if (paddr + 4 <= direct_rdram_size_)
        {
            if (Coverage* coverage = ActiveCoverage())
            {
                coverage->ReadRdram(paddr, 4);
            }
            return xsflib::LoadBe32(rdram_.data() + paddr);
        }
        return ReadMappedRdram32(paddr);
    }
    if (paddr < kCartDomain2Address1)
    {
        return ReadRcp32(paddr);
    }

    return ReadCart32(paddr);
}

uint32_t System::ReadBusPartial32(uint32_t paddr)
{
    if (paddr >= kCartDomain2Address1 && paddr < kPifRom)
    {
        return ReadCart32(paddr & ~1u);
    }

    return ReadBus32(paddr);
}

uint64_t System::ReadBus64(uint32_t paddr)
{
    const uint64_t high = ReadBus32(paddr & ~7u);
    const uint64_t low = ReadBus32((paddr & ~7u) + 4);
    return (high << 32) | low;
}

void System::WriteBus(uint32_t paddr, uint64_t value, int size)
{
    if (paddr < kRdramRegisters)
    {
        if ((mi_.mode & kMiModeRepeat) != 0)
        {
            WriteRdramRepeated(paddr, value, size);
            return;
        }
        if (paddr + size <= direct_rdram_size_)
        {
            uint8_t* p = rdram_.data() + paddr;
            for (int i = size - 1; i >= 0; i--)
            {
                p[i] = static_cast<uint8_t>(value);
                value >>= 8;
            }
            if (Coverage* coverage = ActiveCoverage())
            {
                coverage->WriteRdram(paddr, static_cast<uint32_t>(size));
            }
            return;
        }
        WriteMappedRdram(paddr, value, size);
        return;
    }

    // The debug port is a buffer that takes stores of any size.
    if (!is_viewer_.empty() && paddr >= kIsViewer && paddr + size <= kIsViewer + kIsViewerSize && size < 4)
    {
        uint8_t* p = is_viewer_.data() + (paddr - kIsViewer);
        for (int i = size - 1; i >= 0; i--)
        {
            p[i] = static_cast<uint8_t>(value);
            value >>= 8;
        }
        return;
    }

    // The RCP and the PI take 32-bit writes. A smaller store shifts the whole register to its lanes and writes the
    // low 32 bits of the result: the lanes below it get zeros, the lanes above it the register's next bytes. A
    // doubleword store writes its upper word.
    uint32_t word = 0;
    switch (size)
    {
    case 1:
        word = static_cast<uint32_t>(value << (8 * (3 - (paddr & 3))));
        break;
    case 2:
        word = static_cast<uint32_t>(value << (8 * (2 - (paddr & 2))));
        break;
    case 4:
        word = static_cast<uint32_t>(value);
        break;
    default:
        word = static_cast<uint32_t>(value >> 32);
        break;
    }

    paddr &= ~3u;
    if (paddr < kCartDomain2Address1)
    {
        WriteRcp32(paddr, word);
    }
    else
    {
        WriteCart32(paddr, word);
    }
}

uint32_t System::ReadRcp32(uint32_t paddr)
{
    cpu_->NoteCountUpdated();

    if (paddr < kSpDmem)
    {
        return ReadRdramRegister(paddr);
    }
    if (paddr < kSpRegisters)
    {
        const auto& memory = (paddr & 0x1000) != 0 ? rsp_->Imem() : rsp_->Dmem();
        return xsflib::LoadBe32(memory.data() + (paddr & 0xFFC));
    }
    if (paddr < kSpPcRegister)
    {
        return rsp_->ReadRegister((paddr >> 2) & 7);
    }
    if (paddr < kDpCommandRegisters)
    {
        return (paddr & 4) != 0 ? rsp_->ReadIbist() : rsp_->ReadPc();
    }
    if (paddr < kDpSpanRegisters)
    {
        return ReadDp((paddr >> 2) & 7);
    }
    if (paddr < kMiRegisters)
    {
        return 0;
    }
    if (paddr < kViRegisters)
    {
        return ReadMi((paddr >> 2) & 3);
    }
    if (paddr < kAiRegisters)
    {
        return ReadVi((paddr >> 2) & 15);
    }
    if (paddr < kPiRegisters)
    {
        return ReadAi((paddr >> 2) & 7);
    }
    if (paddr < kRiRegisters)
    {
        return ReadPi((paddr >> 2) & 15);
    }
    if (paddr < kSiRegisters)
    {
        return ReadRi((paddr >> 2) & 7);
    }
    if (paddr < 0x04900000)
    {
        return ReadSi((paddr >> 2) & 7);
    }

    return 0;
}

void System::WriteRcp32(uint32_t paddr, uint32_t value)
{
    cpu_->NoteCountUpdated();

    if (paddr < kSpDmem)
    {
        WriteRdramRegister(paddr, value);
        return;
    }
    if (paddr < kSpRegisters)
    {
        auto& memory = (paddr & 0x1000) != 0 ? rsp_->Imem() : rsp_->Dmem();
        xsflib::StoreBe32(memory.data() + (paddr & 0xFFC), value);
        return;
    }
    if (paddr < kSpPcRegister)
    {
        rsp_->WriteRegister((paddr >> 2) & 7, value);
        return;
    }
    if (paddr < kDpCommandRegisters)
    {
        if ((paddr & 4) != 0)
        {
            rsp_->WriteIbist(value);
        }
        else
        {
            rsp_->WritePc(value);
        }
        return;
    }
    if (paddr < kDpSpanRegisters)
    {
        WriteDp((paddr >> 2) & 7, value);
        return;
    }
    if (paddr < kMiRegisters)
    {
        return;
    }
    if (paddr < kViRegisters)
    {
        WriteMi((paddr >> 2) & 3, value);
        return;
    }
    if (paddr < kAiRegisters)
    {
        WriteVi((paddr >> 2) & 15, value);
        return;
    }
    if (paddr < kPiRegisters)
    {
        WriteAi((paddr >> 2) & 7, value);
        return;
    }
    if (paddr < kRiRegisters)
    {
        WritePi((paddr >> 2) & 15, value);
        return;
    }
    if (paddr < kSiRegisters)
    {
        WriteRi((paddr >> 2) & 7, value);
        return;
    }
    if (paddr < 0x04900000)
    {
        WriteSi((paddr >> 2) & 7, value);
    }
}

uint32_t System::ReadRom32(uint32_t offset)
{
    if (offset >= rom_image_size_)
    {
        const uint32_t low = (kCartRom + offset) & 0xFFFF;
        return (low << 16) | low;
    }

    if (Coverage* coverage = ActiveCoverage())
    {
        coverage->ReadRom(offset, 4);
    }

    if (offset + 4 <= rom_.size())
    {
        return xsflib::LoadBe32(rom_.data() + offset);
    }

    uint32_t word = 0;
    for (uint32_t i = 0; i < 4; i++)
    {
        word <<= 8;
        if (offset + i < rom_.size())
        {
            word |= rom_[offset + i];
        }
    }

    return word;
}

uint32_t System::ReadCart32(uint32_t paddr)
{
    // A direct access through the PI leaves PI_CART_ADDR just past the word it reached, as a DMA does. IPL3's last
    // read of the header leaves 0x1000000C there.
    if (paddr < kPifRom)
    {
        pi_.cart_address = (paddr + 4) & 0xFFFFFFFE;
    }

    // A write to cartridge space is latched, and the next read returns it.
    if (pi_.latch_valid)
    {
        pi_.latch_valid = false;
        if (options_.timing == Timing::kLegacy || now_ < pi_.latch_until)
        {
            return pi_.latch;
        }
    }

    if (paddr >= kPifRam && paddr < kPifRam + kPifRamSize)
    {
        return xsflib::LoadBe32(pif_.Ram().data() + (paddr - kPifRam));
    }
    if (paddr >= kPifRom && paddr < kPifRam)
    {
        // The PIF locks its ROM before the game runs.
        return 0;
    }
    if (!is_viewer_.empty() && paddr >= kIsViewer && paddr < kIsViewer + kIsViewerSize)
    {
        return xsflib::LoadBe32(is_viewer_.data() + (paddr - kIsViewer));
    }
    if (paddr >= kCartRom && paddr < kPifRom)
    {
        return ReadRom32(paddr - kCartRom);
    }

    // Nothing answers: the bus holds the low 16 bits of the address.
    const uint32_t low = paddr & 0xFFFF;
    return (low << 16) | low;
}

void System::WriteCart32(uint32_t paddr, uint32_t value)
{
    if (paddr >= kPifRam && paddr < kPifRam + kPifRamSize)
    {
        xsflib::StoreBe32(pif_.Ram().data() + (paddr - kPifRam), value);
        pif_.OnWrite();
        return;
    }
    if (!is_viewer_.empty() && paddr >= kIsViewer && paddr < kIsViewer + kIsViewerSize)
    {
        WriteIsViewer(paddr - kIsViewer, value);
        return;
    }
    if (paddr >= kPifRom && paddr < kPifRam)
    {
        return;
    }

    if (paddr < kPifRom)
    {
        pi_.cart_address = (paddr + 4) & 0xFFFFFFFE;
    }

    // With the legacy timing, the latch keeps the last write until a read. On the hardware, the first write fills it
    // and marks the PI busy, later writes are lost, and the value goes after about 200 CPU cycles: n64-systemtest's
    // cart-writing tests see it after 10 turns of a three-instruction loop, and not after 110.
    if (options_.timing == Timing::kLegacy)
    {
        pi_.latch = value;
        pi_.latch_valid = true;
        return;
    }
    if (pi_.latch_valid && now_ < pi_.latch_until)
    {
        return;
    }

    pi_.latch = value;
    pi_.latch_valid = true;
    pi_.latch_until = now_ + kCartWriteLatchTicks;
}

void System::WriteIsViewer(uint32_t offset, uint32_t value)
{
    xsflib::StoreBe32(is_viewer_.data() + offset, value);
    if (offset != kIsViewerLength - kIsViewer)
    {
        return;
    }

    const uint32_t start = kIsViewerBuffer - kIsViewer;
    const uint32_t length = std::min<uint32_t>(value, kIsViewerSize - start);
    if (debug_output_ && length > 0)
    {
        debug_output_(std::string_view(reinterpret_cast<const char*>(is_viewer_.data() + start), length));
    }
}

// The API.

Cpu& System::GetCpu()
{
    return *cpu_;
}

const Cpu& System::GetCpu() const
{
    return *cpu_;
}

Rsp& System::GetRsp()
{
    return *rsp_;
}

const Rsp& System::GetRsp() const
{
    return *rsp_;
}

Pif& System::GetPif()
{
    return pif_;
}

std::span<uint8_t> System::Rdram()
{
    return rdram_;
}

std::span<const uint8_t> System::Rdram() const
{
    return rdram_;
}

uint32_t System::ReadPhysical32(uint32_t paddr)
{
    paddr &= ~3u;
    if (paddr < kRdramRegisters)
    {
        return paddr + 4 <= rdram_.size() ? xsflib::LoadBe32(rdram_.data() + paddr) : 0;
    }
    if (paddr >= kSpDmem && paddr < kSpRegisters)
    {
        const auto& memory = (paddr & 0x1000) != 0 ? rsp_->Imem() : rsp_->Dmem();
        return xsflib::LoadBe32(memory.data() + (paddr & 0xFFC));
    }
    if (paddr >= kCartRom && paddr < kPifRom)
    {
        const uint32_t offset = paddr - kCartRom;
        return offset + 4 <= rom_.size() ? xsflib::LoadBe32(rom_.data() + offset) : 0;
    }

    const bool coverage = coverage_enabled_;
    coverage_enabled_ = false;
    const uint32_t value = ReadBus32(paddr);
    coverage_enabled_ = coverage;

    return value;
}

void System::WritePhysical32(uint32_t paddr, uint32_t value)
{
    paddr &= ~3u;
    if (paddr < kRdramRegisters)
    {
        if (paddr + 4 <= rdram_.size())
        {
            xsflib::StoreBe32(rdram_.data() + paddr, value);
        }
        return;
    }

    WriteBus(paddr, value, 4);
}

bool System::ReadVirtual(uint64_t vaddr, std::span<uint8_t> bytes)
{
    for (size_t i = 0; i < bytes.size(); i++)
    {
        const auto paddr = cpu_->TranslateQuiet(vaddr + i, false);
        if (!paddr)
        {
            return false;
        }

        if (*paddr < rdram_.size())
        {
            bytes[i] = rdram_[*paddr];
        }
        else if (*paddr >= kSpDmem && *paddr < kSpRegisters)
        {
            const auto& memory = (*paddr & 0x1000) != 0 ? rsp_->Imem() : rsp_->Dmem();
            bytes[i] = memory[*paddr & 0xFFF];
        }
        else
        {
            return false;
        }
    }

    return true;
}

bool System::WriteVirtual(uint64_t vaddr, std::span<const uint8_t> bytes)
{
    for (size_t i = 0; i < bytes.size(); i++)
    {
        const auto paddr = cpu_->TranslateQuiet(vaddr + i, true);
        if (!paddr)
        {
            return false;
        }

        if (*paddr < rdram_.size())
        {
            rdram_[*paddr] = bytes[i];
        }
        else if (*paddr >= kSpDmem && *paddr < kSpRegisters)
        {
            auto& memory = (*paddr & 0x1000) != 0 ? rsp_->Imem() : rsp_->Dmem();
            memory[*paddr & 0xFFF] = bytes[i];
        }
        else
        {
            return false;
        }
    }

    return true;
}

void System::SetCoverage(bool enabled)
{
    coverage_enabled_ = enabled;
    if (enabled && !coverage_)
    {
        coverage_ = std::make_unique<Coverage>(static_cast<uint32_t>(rdram_.size()));
    }
}

Coverage* System::GetCoverage()
{
    return coverage_.get();
}

void System::SetAudioCallback(std::function<void(const AudioBuffer&)> callback)
{
    audio_callback_ = std::move(callback);
}

void System::SetDebugOutput(std::function<void(std::string_view)> callback)
{
    debug_output_ = std::move(callback);
}

System::Frame System::VideoFrame() const
{
    // VI_STATUS bits 0 and 1 give the pixel format: 2 for 16 bits, 3 for 32 bits.
    const uint32_t type = vi_.registers[0] & 3;
    return {vi_.registers[1] & 0xFFFFFF, vi_.registers[2] & 0xFFF, type == 3 ? 4u : 2u};
}

QuietReport System::CheckQuiet() const
{
    QuietReport report;
    report.cpu_in_delay_slot = cpu_->InDelaySlot();
    report.rsp_running = !rsp_->Halted() || graphics_task_pending_;
    report.sp_interrupt_pending = sp_interrupt_deferred_;
    report.sp_dma_busy = rsp_->DmaBusy() || rsp_->DmaFull();
    report.pi_dma_busy = pi_.busy;
    report.si_dma_busy = si_.busy;
    report.dp_busy = (dp_.status & 0x170) != 0 || dp_.current != dp_.end;
    report.ai_buffers = ai_.count;
    report.ai_buffer_waiting = ai_.count > 1;

    return report;
}

} // namespace usf::n64
