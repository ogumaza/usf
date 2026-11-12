// SPDX-License-Identifier: MIT

// Project64 1.4 save states. Every integer is little-endian. RDRAM, DMEM, IMEM and the copy of the cartridge header are
// in LE word order: a byte at console address A is at A XOR 3. PIF RAM is in console byte order.

#include "n64/save_state.h"

#include <algorithm>
#include <stdexcept>

#include "xsflib/bytes.h"

#include "n64/bits.h"
#include "n64/system.h"

namespace usf::n64
{
namespace
{

constexpr size_t kOffsetRomHeader = 0x008;
constexpr size_t kStateRomHeaderSize = 0x40;
constexpr size_t kOffsetViTimer = 0x048;
constexpr size_t kOffsetPc = 0x04C;
constexpr size_t kOffsetFpr = 0x150;
constexpr size_t kOffsetCop0 = 0x250;
constexpr size_t kOffsetFcr0 = 0x2D0;
constexpr size_t kOffsetFcr31 = 0x34C;
constexpr size_t kOffsetHi = 0x350;
constexpr size_t kOffsetLo = 0x358;
constexpr size_t kOffsetRdramRegisters = 0x360;
constexpr size_t kOffsetSp = 0x388;
constexpr size_t kOffsetDp = 0x3B0;
constexpr size_t kOffsetMi = 0x3D8;
constexpr size_t kOffsetVi = 0x3E8;
constexpr size_t kOffsetAi = 0x420;
constexpr size_t kOffsetPi = 0x438;
constexpr size_t kOffsetRi = 0x46C;
constexpr size_t kOffsetSi = 0x48C;
constexpr size_t kOffsetPifRam = 0x71C;

// Writes the state's little-endian integers at their offsets.
class StateWriter
{
public:
    explicit StateWriter(uint8_t* data) : data_(data)
    {
    }

    void Put32(size_t offset, uint32_t value) const
    {
        xsflib::StoreLe32(data_ + offset, value);
    }

    void Put64(size_t offset, uint64_t value) const
    {
        xsflib::StoreLe64(data_ + offset, value);
    }

private:
    uint8_t* data_;
};

// Reads the state's little-endian integers at their offsets.
class StateReader
{
public:
    explicit StateReader(const uint8_t* data) : data_(data)
    {
    }

    uint32_t Get32(size_t offset) const
    {
        return xsflib::LoadLe32(data_ + offset);
    }

    uint64_t Get64(size_t offset) const
    {
        return xsflib::LoadLe64(data_ + offset);
    }

private:
    const uint8_t* data_;
};

// The FPU's registers in the state. With FR set, slot n holds register n. With FR clear, slot k holds even register 2k,
// whose halves are the singles 2k and 2k + 1, and slots 16 to 31 are zero. The odd physical registers aren't kept.
std::array<uint64_t, 32> FprToSlots(const std::array<uint64_t, 32>& fpr, bool fr)
{
    if (fr)
    {
        return fpr;
    }

    std::array<uint64_t, 32> slots{};
    for (int k = 0; k < 16; k++)
    {
        slots[k] = fpr[2 * k];
    }

    return slots;
}

std::array<uint64_t, 32> SlotsToFpr(const std::array<uint64_t, 32>& slots, bool fr)
{
    if (fr)
    {
        return slots;
    }

    std::array<uint64_t, 32> fpr{};
    for (int k = 0; k < 16; k++)
    {
        fpr[2 * k] = slots[k];
    }

    return fpr;
}

} // namespace

std::vector<uint8_t> System::SaveState() const
{
    const size_t rdram_size = rdram_.size();
    std::vector<uint8_t> state(kStateOverhead + rdram_size, 0);
    uint8_t* p = state.data();
    const StateWriter out(p);

    out.Put32(0, kStateId);
    out.Put32(kStateRdramSizeOffset, static_cast<uint32_t>(rdram_size));
    std::array<uint8_t, kStateRomHeaderSize> header{};
    for (size_t i = 0; i < header.size() && i < rom_.size(); i++)
    {
        header[i] = rom_[i];
    }
    CopyToLeWords(header.data(), p + kOffsetRomHeader, header.size());

    // The VI timer counts in Count's units, which are two ticks.
    const uint64_t next_vi = events_[static_cast<int>(Event::kVi)];
    out.Put32(kOffsetViTimer, next_vi == kNever || next_vi < now_ ? 0 : static_cast<uint32_t>((next_vi - now_) / 2));

    SaveCpu(p);
    SaveRcp(p);

    // PIF RAM is the one memory that the states hold in console byte order.
    std::copy_n(pif_.Ram().data(), kPifRamSize, p + kOffsetPifRam);
    CopyToLeWords(rdram_.data(), p + kStateRdramOffset, rdram_size);
    CopyToLeWords(rsp_->Dmem().data(), p + kStateRdramOffset + rdram_size, kSpMemSize);
    CopyToLeWords(rsp_->Imem().data(), p + kStateRdramOffset + rdram_size + kSpMemSize, kSpMemSize);

    return state;
}

void System::LoadState(std::span<const uint8_t> state)
{
    if (state.size() < kStateRdramOffset)
    {
        throw std::runtime_error("the save state is too short");
    }

    const uint8_t* p = state.data();
    const StateReader in(p);
    if (in.Get32(0) != kStateId)
    {
        throw std::runtime_error("the save state's identifier is wrong");
    }
    const uint32_t rdram_size = in.Get32(kStateRdramSizeOffset);
    if (rdram_size != kRdramSize4 && rdram_size != kRdramSize8)
    {
        throw std::runtime_error("the save state's RDRAM size is neither 4 nor 8 MiB");
    }
    if (state.size() < kStateOverhead + rdram_size)
    {
        throw std::runtime_error("the save state is shorter than its RDRAM size needs");
    }

    options_.rdram_size = rdram_size;
    Reset();

    // The region comes from the country code in the state's copy of the header.
    std::array<uint8_t, kStateRomHeaderSize> header{};
    CopyFromLeWords(p + kOffsetRomHeader, header.data(), header.size());
    tv_ = IsPalCountry(header[kHeaderCountryCode]) ? TvType::kPal : TvType::kNtsc;

    std::copy_n(p + kOffsetPifRam, kPifRamSize, pif_.Ram().data());
    pif_.OnStateLoaded();
    CopyFromLeWords(p + kStateRdramOffset, rdram_.data(), rdram_size);
    CopyFromLeWords(p + kStateRdramOffset + rdram_size, rsp_->Dmem().data(), kSpMemSize);
    CopyFromLeWords(p + kStateRdramOffset + rdram_size + kSpMemSize, rsp_->Imem().data(), kSpMemSize);

    LoadCpu(p);
    LoadRcp(p);

    // The legacy timing leaves Cause's IP2 as the state holds it until the first re-check of the interrupt lines. That
    // bit takes no interrupt.
    if (LateEvents())
    {
        cpu_->MarkInterruptLineStale();
    }
    else
    {
        UpdateCpuInterrupt();
    }
}

void System::SaveCpu(uint8_t* state) const
{
    const StateWriter out(state);

    const CpuState cpu = cpu_->State();
    out.Put32(kOffsetPc, static_cast<uint32_t>(cpu.pc));
    for (int i = 0; i < 32; i++)
    {
        out.Put64(kStateGprOffset + 8 * i, cpu.gpr[i]);
    }
    const bool fr = (cpu.cop0[kCop0Status] & kStatusFr) != 0;
    const std::array<uint64_t, 32> slots = FprToSlots(cpu.fpr, fr);
    for (int i = 0; i < 32; i++)
    {
        out.Put64(kOffsetFpr + 8 * i, slots[i]);
        out.Put32(kOffsetCop0 + 4 * i, static_cast<uint32_t>(cpu.cop0[i]));
    }
    out.Put32(kOffsetFcr0, cpu.fcr0);
    out.Put32(kOffsetFcr31, cpu.fcr31);
    out.Put64(kOffsetHi, cpu.hi);
    out.Put64(kOffsetLo, cpu.lo);

    // Every TLB entry is written as defined.
    for (int i = 0; i < 32; i++)
    {
        const TlbEntry& entry = cpu_->Tlb(i);
        const size_t offset = kStateTlbOffset + 20 * i;
        out.Put32(offset, 1);
        out.Put32(offset + 4, entry.page_mask);
        out.Put32(offset + 8, static_cast<uint32_t>(entry.entry_hi));
        out.Put32(offset + 12, entry.entry_lo0);
        out.Put32(offset + 16, entry.entry_lo1);
    }
}

void System::SaveRcp(uint8_t* state) const
{
    const StateWriter out(state);

    const RdramChip& chip = rdram_chips_[0];
    const std::array<uint32_t, 10> rdram_registers = {
        chip.device_type, chip.device_id_register, chip.delay,        chip.mode,           chip.ref_interval,
        chip.ref_row,     chip.ras_interval,       chip.min_interval, chip.address_select, chip.device_manufacturer,
    };
    for (size_t i = 0; i < rdram_registers.size(); i++)
    {
        out.Put32(kOffsetRdramRegisters + 4 * i, rdram_registers[i]);
    }

    const SpRegisters sp = rsp_->Registers();
    const std::array<uint32_t, 10> sp_registers = {
        sp.mem_address, sp.dram_address, sp.read_length, sp.write_length, sp.status,
        sp.dma_full,    sp.dma_busy,     sp.semaphore,   sp.pc,           sp.ibist,
    };
    for (size_t i = 0; i < sp_registers.size(); i++)
    {
        out.Put32(kOffsetSp + 4 * i, sp_registers[i]);
    }

    const std::array<uint32_t, 8> dp_registers = {
        dp_.start, dp_.end, dp_.current, dp_.status, dp_.clock, dp_.buffer_busy, dp_.pipe_busy, dp_.tmem,
    };
    for (size_t i = 0; i < dp_registers.size(); i++)
    {
        out.Put32(kOffsetDp + 4 * i, dp_registers[i]);
    }

    out.Put32(kOffsetMi, mi_.mode);
    out.Put32(kOffsetMi + 4, mi_.version);
    out.Put32(kOffsetMi + 8, mi_.interrupt);
    out.Put32(kOffsetMi + 12, mi_.mask);

    for (uint32_t i = 0; i < Vi::kRegisterCount; i++)
    {
        out.Put32(kOffsetVi + 4 * i, vi_.registers[i]);
    }

    // AI_LEN holds the bytes left in the buffer that plays. AI_STATUS holds busy and full.
    uint32_t ai_length = 0;
    if (ai_.count > 0)
    {
        const Ai::Buffer& buffer = ai_.buffers[0];
        if (buffer.started && buffer.duration > 0 && now_ < buffer.start + buffer.duration)
        {
            ai_length = AiBytesLeft(buffer);
        }
    }

    const std::array<uint32_t, 6> ai_registers = {
        ai_.dram_address, ai_length, ai_.control, AiStatus(), ai_.dac_rate, ai_.bit_rate,
    };
    for (size_t i = 0; i < ai_registers.size(); i++)
    {
        out.Put32(kOffsetAi + 4 * i, ai_registers[i]);
    }

    const std::array<uint32_t, 5> pi_registers = {
        pi_.dram_address, pi_.cart_address, pi_.read_length, pi_.write_length, PiStatus(),
    };
    for (size_t i = 0; i < pi_registers.size(); i++)
    {
        out.Put32(kOffsetPi + 4 * i, pi_registers[i]);
    }
    for (size_t i = 0; i < pi_.domains.size(); i++)
    {
        out.Put32(kOffsetPi + 20 + 4 * i, pi_.domains[i]);
    }

    for (size_t i = 0; i < ri_.registers.size(); i++)
    {
        out.Put32(kOffsetRi + 4 * i, ri_.registers[i]);
    }

    out.Put32(kOffsetSi, si_.dram_address);
    out.Put32(kOffsetSi + 4, si_.pif_address_read);
    out.Put32(kOffsetSi + 8, si_.pif_address_write);
    out.Put32(kOffsetSi + 12, SiStatus());
}

void System::LoadCpu(const uint8_t* state)
{
    const StateReader in(state);

    // The CPU. 32-bit COP0 values are sign-extended, as MTC0 would write them.
    CpuState cpu;
    cpu.pc = SignExtend32(in.Get32(kOffsetPc));
    std::array<uint64_t, 32> slots{};
    for (int i = 0; i < 32; i++)
    {
        cpu.gpr[i] = in.Get64(kStateGprOffset + 8 * i);
        slots[i] = in.Get64(kOffsetFpr + 8 * i);
        cpu.cop0[i] = SignExtend32(in.Get32(kOffsetCop0 + 4 * i));
    }
    const bool fr = (cpu.cop0[kCop0Status] & kStatusFr) != 0;
    cpu.fpr = SlotsToFpr(slots, fr);
    cpu.fcr0 = in.Get32(kOffsetFcr0);
    cpu.fcr31 = in.Get32(kOffsetFcr31);
    cpu.hi = in.Get64(kOffsetHi);
    cpu.lo = in.Get64(kOffsetLo);
    cpu_->SetState(cpu);

    // The TLB, loaded as TLBWI would.
    for (int i = 0; i < 32; i++)
    {
        const size_t offset = kStateTlbOffset + 20 * i;
        TlbEntry entry;
        if (in.Get32(offset) != 0 || options_.timing == Timing::kLegacy)
        {
            entry.page_mask = in.Get32(offset + 4);
            entry.entry_hi = SignExtend32(in.Get32(offset + 8));
            entry.entry_lo0 = in.Get32(offset + 12);
            entry.entry_lo1 = in.Get32(offset + 16);
            entry.global = (entry.entry_lo0 & entry.entry_lo1 & 1) != 0;
        }

        cpu_->SetTlb(i, entry);
    }
}

void System::LoadRcp(const uint8_t* state)
{
    const StateReader in(state);

    // A state comes from a running program, so IPL3 has set the chips up as plain memory. Reset() has left them that
    // way. The loop below reads their registers from the state.
    for (RdramChip& chip : rdram_chips_)
    {
        if (!chip.present)
        {
            continue;
        }

        chip.device_type = in.Get32(kOffsetRdramRegisters);
        chip.device_id_register = in.Get32(kOffsetRdramRegisters + 4);
        chip.delay = in.Get32(kOffsetRdramRegisters + 8);
        chip.mode = in.Get32(kOffsetRdramRegisters + 12);
        chip.ref_interval = in.Get32(kOffsetRdramRegisters + 16);
        chip.ref_row = in.Get32(kOffsetRdramRegisters + 20);
        chip.ras_interval = in.Get32(kOffsetRdramRegisters + 24);
        chip.min_interval = in.Get32(kOffsetRdramRegisters + 28);
        chip.address_select = in.Get32(kOffsetRdramRegisters + 32);
        chip.device_manufacturer = in.Get32(kOffsetRdramRegisters + 36);
    }

    SpRegisters sp;
    sp.mem_address = in.Get32(kOffsetSp);
    sp.dram_address = in.Get32(kOffsetSp + 4);
    sp.read_length = in.Get32(kOffsetSp + 8);
    sp.write_length = in.Get32(kOffsetSp + 12);
    sp.status = in.Get32(kOffsetSp + 16);
    sp.dma_full = in.Get32(kOffsetSp + 20);
    sp.dma_busy = in.Get32(kOffsetSp + 24);
    sp.semaphore = in.Get32(kOffsetSp + 28);
    sp.pc = in.Get32(kOffsetSp + 32);
    sp.ibist = in.Get32(kOffsetSp + 36);
    rsp_->SetRegisters(sp);

    dp_.start = in.Get32(kOffsetDp);
    dp_.end = in.Get32(kOffsetDp + 4);
    dp_.current = in.Get32(kOffsetDp + 8);
    dp_.status = in.Get32(kOffsetDp + 12);
    dp_.clock = in.Get32(kOffsetDp + 16);
    dp_.buffer_busy = in.Get32(kOffsetDp + 20);
    dp_.pipe_busy = in.Get32(kOffsetDp + 24);
    dp_.tmem = in.Get32(kOffsetDp + 28);

    // Project64's states keep the init mode bit that the boot code set, and the legacy players never repeat a write. A
    // loaded state therefore leaves repeat mode off. Otherwise the game's next RDRAM store would write one byte.
    mi_.mode = in.Get32(kOffsetMi) & ~kMiModeRepeat;
    mi_.interrupt = in.Get32(kOffsetMi + 8) & kMiInterruptAll;
    mi_.mask = in.Get32(kOffsetMi + 12) & kMiInterruptAll;

    for (uint32_t i = 0; i < Vi::kRegisterCount; i++)
    {
        vi_.registers[i] = in.Get32(kOffsetVi + 4 * i);
    }

    // The next vertical interrupt comes after the state's VI timer, in Count's units. With the legacy timing, a timer
    // of 0 makes it due at once.
    const uint32_t vi_timer = in.Get32(kOffsetViTimer);
    if (vi_timer != 0 || LateEvents())
    {
        vi_.field_ticks = FieldTicks();
        vi_.next_interrupt = now_ + 2 * uint64_t{vi_timer};
        Schedule(Event::kVi, vi_.next_interrupt);
    }
    else
    {
        StartVi();
    }

    // AI_STATUS starts clear. With the AI's delay, a non-zero AI_LEN describes a buffer that plays from now on, whose
    // samples have already been taken. AI_LEN reads 0 while it plays. Without the delay, the legacy players start
    // nothing.
    ai_.dram_address = in.Get32(kOffsetAi);
    ai_.control = in.Get32(kOffsetAi + 8) & kAiControlDma;
    ai_.dac_rate = in.Get32(kOffsetAi + 16) & kAiDacRateMask;
    ai_.bit_rate = in.Get32(kOffsetAi + 20) & kAiBitRateMask;
    ai_.length = in.Get32(kOffsetAi + 4) & kAiLengthMask;
    if (ai_.length != 0 && AiDelay())
    {
        ai_.count = 1;
        Ai::Buffer& buffer = ai_.buffers[0];
        buffer = {ai_.dram_address, ai_.length, now_, std::max<uint64_t>(AiBufferTicks(ai_.length), 1)};
        buffer.started = true;
        buffer.from_state = true;
        Schedule(Event::kAiBuffer, now_ + buffer.duration);
    }

    pi_.dram_address = in.Get32(kOffsetPi) & kPiDramAddressMask;
    pi_.cart_address = in.Get32(kOffsetPi + 4) & kPiCartAddressMask;
    pi_.read_length = in.Get32(kOffsetPi + 8);
    pi_.write_length = in.Get32(kOffsetPi + 12);
    for (size_t i = 0; i < pi_.domains.size(); i++)
    {
        pi_.domains[i] = in.Get32(kOffsetPi + 20 + 4 * i);
    }

    for (size_t i = 0; i < ri_.registers.size(); i++)
    {
        ri_.registers[i] = in.Get32(kOffsetRi + 4 * i);
    }

    si_.dram_address = in.Get32(kOffsetSi);
    si_.pif_address_read = in.Get32(kOffsetSi + 4);
    si_.pif_address_write = in.Get32(kOffsetSi + 8);
}

} // namespace usf::n64
