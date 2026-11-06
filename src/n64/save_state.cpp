// SPDX-License-Identifier: MIT

// Project64 1.4 save states. Every integer is little-endian. RDRAM, DMEM, IMEM and the copy of the cartridge header are
// in LE word order: a byte at console address A is at A XOR 3. PIF RAM is in console byte order.

#include <algorithm>
#include <stdexcept>

#include "xsflib/bytes.h"

#include "n64/system.h"

namespace usf::n64
{

namespace
{

constexpr uint32_t kStateId = 0x23D8A6C8;
constexpr size_t kHeaderSize = 0x75C;
constexpr size_t kOffsetRdramSize = 0x004;
constexpr size_t kOffsetRomHeader = 0x008;
constexpr size_t kOffsetViTimer = 0x048;
constexpr size_t kOffsetPc = 0x04C;
constexpr size_t kOffsetGpr = 0x050;
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
constexpr size_t kOffsetTlb = 0x49C;
constexpr size_t kOffsetPifRam = 0x71C;
constexpr size_t kOffsetRdram = 0x75C;

uint64_t SignExtend32(uint32_t value)
{
    return static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(value)));
}

// Copies memory in console order to or from LE word order.
void CopyToLeWords(const uint8_t* source, uint8_t* target, size_t size)
{
    for (size_t i = 0; i < size; i++)
    {
        target[i ^ 3] = source[i];
    }
}

void CopyFromLeWords(const uint8_t* source, uint8_t* target, size_t size)
{
    for (size_t i = 0; i < size; i++)
    {
        target[i] = source[i ^ 3];
    }
}

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
    std::vector<uint8_t> state(kHeaderSize + rdram_size + 2 * kSpMemSize, 0);
    uint8_t* p = state.data();
    auto put32 = [p](size_t offset, uint32_t value)
    {
        xsflib::StoreLe32(p + offset, value);
    };

    auto put64 = [p](size_t offset, uint64_t value)
    {
        xsflib::StoreLe64(p + offset, value);
    };

    put32(0, kStateId);
    put32(kOffsetRdramSize, static_cast<uint32_t>(rdram_size));
    std::array<uint8_t, 0x40> header{};
    for (size_t i = 0; i < header.size() && i < rom_.size(); i++)
    {
        header[i] = rom_[i];
    }
    CopyToLeWords(header.data(), p + kOffsetRomHeader, header.size());

    // The VI timer counts in Count's units, which are two ticks.
    const uint64_t next_vi = events_[static_cast<int>(Event::kVi)];
    put32(kOffsetViTimer, next_vi == kNever || next_vi < now_ ? 0 : static_cast<uint32_t>((next_vi - now_) / 2));

    const CpuState cpu = cpu_->State();
    put32(kOffsetPc, static_cast<uint32_t>(cpu.pc));
    for (int i = 0; i < 32; i++)
    {
        put64(kOffsetGpr + 8 * i, cpu.gpr[i]);
    }
    const bool fr = (cpu.cop0[kCop0Status] & kStatusFr) != 0;
    const std::array<uint64_t, 32> slots = FprToSlots(cpu.fpr, fr);
    for (int i = 0; i < 32; i++)
    {
        put64(kOffsetFpr + 8 * i, slots[i]);
        put32(kOffsetCop0 + 4 * i, static_cast<uint32_t>(cpu.cop0[i]));
    }
    put32(kOffsetFcr0, cpu.fcr0);
    put32(kOffsetFcr31, cpu.fcr31);
    put64(kOffsetHi, cpu.hi);
    put64(kOffsetLo, cpu.lo);

    const RdramChip& chip = rdram_chips_[0];
    const std::array<uint32_t, 10> rdram_registers = {
        chip.device_type, chip.device_id_register, chip.delay,        chip.mode,           chip.ref_interval,
        chip.ref_row,     chip.ras_interval,       chip.min_interval, chip.address_select, chip.device_manufacturer,
    };
    for (int i = 0; i < 10; i++)
    {
        put32(kOffsetRdramRegisters + 4 * i, rdram_registers[i]);
    }

    const SpRegisters sp = rsp_->Registers();
    const std::array<uint32_t, 10> sp_registers = {
        sp.mem_address, sp.dram_address, sp.read_length, sp.write_length, sp.status,
        sp.dma_full,    sp.dma_busy,     sp.semaphore,   sp.pc,           sp.ibist,
    };
    for (int i = 0; i < 10; i++)
    {
        put32(kOffsetSp + 4 * i, sp_registers[i]);
    }

    const std::array<uint32_t, 8> dp_registers = {
        dp_.start, dp_.end, dp_.current, dp_.status, dp_.clock, dp_.buffer_busy, dp_.pipe_busy, dp_.tmem,
    };
    for (int i = 0; i < 8; i++)
    {
        put32(kOffsetDp + 4 * i, dp_registers[i]);
    }

    put32(kOffsetMi, mi_.mode);
    put32(kOffsetMi + 4, mi_.version);
    put32(kOffsetMi + 8, mi_.interrupt);
    put32(kOffsetMi + 12, mi_.mask);

    for (int i = 0; i < 14; i++)
    {
        put32(kOffsetVi + 4 * i, vi_.registers[i]);
    }

    // AI_LEN holds the bytes left in the buffer that plays. AI_STATUS holds busy and full.
    uint32_t ai_length = 0;
    if (ai_.count > 0)
    {
        const Ai::Buffer& buffer = ai_.buffers[0];
        if (buffer.started && buffer.duration > 0 && now_ < buffer.start + buffer.duration)
        {
            const uint64_t left = buffer.start + buffer.duration - now_;
            ai_length = static_cast<uint32_t>(uint64_t{buffer.length} * left / buffer.duration) & ~7u;
        }
    }

    uint32_t ai_status = (ai_.count > 0 ? 0x40000000u : 0) | (ai_.count > 1 ? 0x80000001u : 0);
    if ((ai_.control & 1) != 0)
    {
        ai_status |= 0x02000000;
    }

    const std::array<uint32_t, 6> ai_registers = {
        ai_.dram_address, ai_length, ai_.control, ai_status, ai_.dac_rate, ai_.bit_rate,
    };
    for (int i = 0; i < 6; i++)
    {
        put32(kOffsetAi + 4 * i, ai_registers[i]);
    }

    uint32_t pi_status = (pi_.busy ? 1u : 0) | (pi_.error ? 4u : 0) | ((mi_.interrupt & kMiInterruptPi) != 0 ? 8u : 0);
    const std::array<uint32_t, 5> pi_registers = {
        pi_.dram_address, pi_.cart_address, pi_.read_length, pi_.write_length, pi_status,
    };
    for (int i = 0; i < 5; i++)
    {
        put32(kOffsetPi + 4 * i, pi_registers[i]);
    }
    for (int i = 0; i < 8; i++)
    {
        put32(kOffsetPi + 20 + 4 * i, pi_.domains[i]);
    }

    for (int i = 0; i < 8; i++)
    {
        put32(kOffsetRi + 4 * i, ri_.registers[i]);
    }

    const uint32_t si_status = (si_.busy ? 1u : 0) | ((mi_.interrupt & kMiInterruptSi) != 0 ? 1u << 12 : 0);
    put32(kOffsetSi, si_.dram_address);
    put32(kOffsetSi + 4, si_.pif_address_read);
    put32(kOffsetSi + 8, si_.pif_address_write);
    put32(kOffsetSi + 12, si_status);

    // Every TLB entry is written as defined.
    for (int i = 0; i < 32; i++)
    {
        const TlbEntry& entry = cpu_->Tlb(i);
        const size_t offset = kOffsetTlb + 20 * i;
        put32(offset, 1);
        put32(offset + 4, entry.page_mask);
        put32(offset + 8, static_cast<uint32_t>(entry.entry_hi));
        put32(offset + 12, entry.entry_lo0);
        put32(offset + 16, entry.entry_lo1);
    }

    // PIF RAM is the one memory that the states hold in console byte order.
    std::copy_n(pif_.Ram().data(), kPifRamSize, p + kOffsetPifRam);
    CopyToLeWords(rdram_.data(), p + kOffsetRdram, rdram_size);
    CopyToLeWords(rsp_->Dmem().data(), p + kOffsetRdram + rdram_size, kSpMemSize);
    CopyToLeWords(rsp_->Imem().data(), p + kOffsetRdram + rdram_size + kSpMemSize, kSpMemSize);

    return state;
}

void System::LoadState(std::span<const uint8_t> state)
{
    if (state.size() < kHeaderSize)
    {
        throw std::runtime_error("the save state is too short");
    }

    const uint8_t* p = state.data();
    auto get32 = [p](size_t offset)
    {
        return xsflib::LoadLe32(p + offset);
    };

    auto get64 = [p](size_t offset)
    {
        return xsflib::LoadLe64(p + offset);
    };

    if (get32(0) != kStateId)
    {
        throw std::runtime_error("the save state's identifier is wrong");
    }
    const uint32_t rdram_size = get32(kOffsetRdramSize);
    if (rdram_size != kRdramSize4 && rdram_size != kRdramSize8)
    {
        throw std::runtime_error("the save state's RDRAM size is neither 4 nor 8 MiB");
    }
    if (state.size() < kHeaderSize + rdram_size + 2 * kSpMemSize)
    {
        throw std::runtime_error("the save state is shorter than its RDRAM size needs");
    }

    options_.rdram_size = rdram_size;
    Reset();

    // The region comes from the country code in the state's copy of the header.
    std::array<uint8_t, 0x40> header{};
    CopyFromLeWords(p + kOffsetRomHeader, header.data(), header.size());
    switch (header[0x3E])
    {
    case 0x44:
    case 0x46:
    case 0x49:
    case 0x50:
    case 0x53:
    case 0x55:
    case 0x58:
    case 0x59:
        tv_ = TvType::kPal;
        break;
    default:
        tv_ = TvType::kNtsc;
        break;
    }

    std::copy_n(p + kOffsetPifRam, kPifRamSize, pif_.Ram().data());
    pif_.OnStateLoaded();
    CopyFromLeWords(p + kOffsetRdram, rdram_.data(), rdram_size);
    CopyFromLeWords(p + kOffsetRdram + rdram_size, rsp_->Dmem().data(), kSpMemSize);
    CopyFromLeWords(p + kOffsetRdram + rdram_size + kSpMemSize, rsp_->Imem().data(), kSpMemSize);

    // The CPU. 32-bit COP0 values are sign-extended, as MTC0 would write them.
    CpuState cpu;
    cpu.pc = SignExtend32(get32(kOffsetPc));
    std::array<uint64_t, 32> slots{};
    for (int i = 0; i < 32; i++)
    {
        cpu.gpr[i] = get64(kOffsetGpr + 8 * i);
        slots[i] = get64(kOffsetFpr + 8 * i);
        cpu.cop0[i] = SignExtend32(get32(kOffsetCop0 + 4 * i));
    }
    const bool fr = (cpu.cop0[kCop0Status] & kStatusFr) != 0;
    cpu.fpr = SlotsToFpr(slots, fr);
    cpu.fcr0 = get32(kOffsetFcr0);
    cpu.fcr31 = get32(kOffsetFcr31);
    cpu.hi = get64(kOffsetHi);
    cpu.lo = get64(kOffsetLo);
    cpu_->SetState(cpu);
    cpu_->SetFcr31(cpu.fcr31);

    // The TLB, loaded as TLBWI would.
    for (int i = 0; i < 32; i++)
    {
        const size_t offset = kOffsetTlb + 20 * i;
        TlbEntry entry;
        if (get32(offset) != 0 || options_.timing == Timing::kLegacy)
        {
            entry.page_mask = get32(offset + 4);
            entry.entry_hi = SignExtend32(get32(offset + 8));
            entry.entry_lo0 = get32(offset + 12);
            entry.entry_lo1 = get32(offset + 16);
            entry.global = (entry.entry_lo0 & entry.entry_lo1 & 1) != 0;
        }

        cpu_->SetTlb(i, entry);
    }

    for (RdramChip& chip : rdram_chips_)
    {
        if (!chip.present)
        {
            continue;
        }

        chip.device_type = get32(kOffsetRdramRegisters);
        chip.device_id_register = get32(kOffsetRdramRegisters + 4);
        chip.delay = get32(kOffsetRdramRegisters + 8);
        chip.mode = get32(kOffsetRdramRegisters + 12);
        chip.ref_interval = get32(kOffsetRdramRegisters + 16);
        chip.ref_row = get32(kOffsetRdramRegisters + 20);
        chip.ras_interval = get32(kOffsetRdramRegisters + 24);
        chip.min_interval = get32(kOffsetRdramRegisters + 28);
        chip.address_select = get32(kOffsetRdramRegisters + 32);
        chip.device_manufacturer = get32(kOffsetRdramRegisters + 36);
    }

    // A state comes from a running program, so IPL3 has set the chips up as plain memory.
    for (size_t n = 0; n < rdram_chips_.size(); n++)
    {
        RdramChip& chip = rdram_chips_[n];
        chip.enable = chip.present;
        chip.device_id = static_cast<uint32_t>(2 * n);
        chip.write_delay = 1;
        chip.current = 63;
    }
    UpdateRdramMapping();

    SpRegisters sp;
    sp.mem_address = get32(kOffsetSp);
    sp.dram_address = get32(kOffsetSp + 4);
    sp.read_length = get32(kOffsetSp + 8);
    sp.write_length = get32(kOffsetSp + 12);
    sp.status = get32(kOffsetSp + 16);
    sp.dma_full = get32(kOffsetSp + 20);
    sp.dma_busy = get32(kOffsetSp + 24);
    sp.semaphore = get32(kOffsetSp + 28);
    sp.pc = get32(kOffsetSp + 32);
    sp.ibist = get32(kOffsetSp + 36);
    rsp_->SetRegisters(sp);

    dp_.start = get32(kOffsetDp);
    dp_.end = get32(kOffsetDp + 4);
    dp_.current = get32(kOffsetDp + 8);
    dp_.status = get32(kOffsetDp + 12);
    dp_.clock = get32(kOffsetDp + 16);
    dp_.buffer_busy = get32(kOffsetDp + 20);
    dp_.pipe_busy = get32(kOffsetDp + 24);
    dp_.tmem = get32(kOffsetDp + 28);

    // Project64's states keep the init mode bit that the boot code set, and the legacy players never repeat a write.
    // A loaded state therefore leaves repeat mode off. Otherwise the game's next RDRAM store would write one byte.
    mi_.mode = get32(kOffsetMi) & ~kMiModeRepeat;
    mi_.interrupt = get32(kOffsetMi + 8) & 0x3F;
    mi_.mask = get32(kOffsetMi + 12) & 0x3F;

    for (int i = 0; i < 14; i++)
    {
        vi_.registers[i] = get32(kOffsetVi + 4 * i);
    }

    // The next vertical interrupt comes after the state's VI timer, in Count's units. With the legacy timing, a timer
    // of 0 makes it due at once.
    const uint32_t vi_timer = get32(kOffsetViTimer);
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
    ai_.dram_address = get32(kOffsetAi);
    ai_.control = get32(kOffsetAi + 8) & 1;
    ai_.dac_rate = get32(kOffsetAi + 16) & 0x3FFF;
    ai_.bit_rate = get32(kOffsetAi + 20) & 0xF;
    ai_.length = get32(kOffsetAi + 4) & 0x3FFF8;
    if (ai_.length != 0 && AiDelay())
    {
        ai_.count = 1;
        Ai::Buffer& buffer = ai_.buffers[0];
        buffer = {ai_.dram_address, ai_.length, now_, std::max<uint64_t>(AiBufferTicks(ai_.length), 1)};
        buffer.started = true;
        buffer.from_state = true;
        Schedule(Event::kAiBuffer, now_ + buffer.duration);
    }

    pi_.dram_address = get32(kOffsetPi) & 0xFFFFFE;
    pi_.cart_address = get32(kOffsetPi + 4) & 0xFFFFFFFE;
    pi_.read_length = get32(kOffsetPi + 8);
    pi_.write_length = get32(kOffsetPi + 12);
    for (int i = 0; i < 8; i++)
    {
        pi_.domains[i] = get32(kOffsetPi + 20 + 4 * i);
    }

    for (int i = 0; i < 8; i++)
    {
        ri_.registers[i] = get32(kOffsetRi + 4 * i);
    }

    si_.dram_address = get32(kOffsetSi);
    si_.pif_address_read = get32(kOffsetSi + 4);
    si_.pif_address_write = get32(kOffsetSi + 8);

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

} // namespace usf::n64
