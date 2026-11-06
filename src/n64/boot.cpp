// SPDX-License-Identifier: MIT

// Boot. The PIF's boot code (IPL1 and IPL2) isn't run: the system starts in the state it leaves for the cartridge's
// IPL3, which then runs as on the hardware. The facts come from the n64brew wiki's pages on the PIF and on the
// initial program load.

#include <array>

#include "xsflib/bytes.h"

#include "n64/system.h"

namespace usf::n64
{

namespace
{

// The CPU's general-purpose registers by number.
constexpr int kRegT3 = 11;
constexpr int kRegS3 = 19;
constexpr int kRegS4 = 20;
constexpr int kRegS5 = 21;
constexpr int kRegS6 = 22;
constexpr int kRegS7 = 23;
constexpr int kRegSp = 29;
constexpr int kRegRa = 31;

// IPL2 occupies the PIF ROM from offset 0xD4 to 0x71B. IPL1 copies it to the start of IMEM.
constexpr uint32_t kIpl2Start = 0xD4;
constexpr uint32_t kIpl2End = 0x71C;

uint64_t SignExtend32(uint32_t value)
{
    return static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(value)));
}

struct Candidate
{
    Cic cic;
    uint8_t seed;
    uint32_t entry_offset; // IPL3 subtracts this from the header's entry point.
};

// The seeds of the boot chips, from the n64brew wiki. The 6101 shares the 6102's seed. The 7102 jumps to a fixed
// address.
constexpr std::array<Candidate, 4> kCandidates = {{
    {Cic::k6102, 0x3F, 0},
    {Cic::k6103, 0x78, 0x100000},
    {Cic::k6105, 0x91, 0},
    {Cic::k6106, 0x85, 0x200000},
}};

constexpr uint64_t kFixedEntry7102 = 0xFFFFFFFF80000480ull;

// The most that IPL3 runs for before it gives up. A cold boot takes a fraction of a second.
constexpr uint64_t kBootBudgetTicks = 4 * kCpuClock;

} // namespace

void System::PrepareIpl3(uint8_t ipl3_seed, bool version_bit)
{
    Reset();
    ResetRdramChips(false);

    // IPL2 copies IPL3 from the cartridge into DMEM, and leaves itself in IMEM.
    auto& dmem = rsp_->Dmem();
    for (uint32_t i = 0x40; i < 0x1000; i++)
    {
        dmem[i] = i < rom_.size() ? rom_[i] : 0;
    }

    auto& imem = rsp_->Imem();
    if (pif_rom_.size() >= kIpl2End)
    {
        std::copy(pif_rom_.begin() + kIpl2Start, pif_rom_.begin() + kIpl2End, imem.begin());
    }

    // The PI's timing for domain 1 comes from the header's first word.
    const uint32_t header = rom_.size() >= 4 ? xsflib::LoadBe32(rom_.data()) : 0;
    pi_.domains[0] = header & 0xFF;
    pi_.domains[1] = (header >> 8) & 0xFF;
    pi_.domains[2] = (header >> 16) & 0x0F;
    pi_.domains[3] = (header >> 20) & 0x03;

    vi_.registers[3] = 0x3FF;
    ScheduleVi();

    // The registers that IPL3 reads: the television system, the reset type, the seed and the region's version bits.
    const uint32_t region = tv_ == TvType::kPal ? 6 : (tv_ == TvType::kMpal ? 4 : 0);
    cpu_->SetGpr(kRegS3, 0);
    cpu_->SetGpr(kRegS4, static_cast<uint32_t>(tv_));
    cpu_->SetGpr(kRegS5, 0);
    cpu_->SetGpr(kRegS6, ipl3_seed);
    cpu_->SetGpr(kRegS7, (version_bit ? 1u : 0u) | region);
    cpu_->SetGpr(kRegSp, SignExtend32(0xA4001FF0));
    cpu_->SetGpr(kRegT3, SignExtend32(0xA4000040));
    cpu_->SetGpr(kRegRa, SignExtend32(tv_ == TvType::kNtsc ? 0xA4001550 : 0xA4001554));
    cpu_->SetPc(SignExtend32(0xA4000040));
}

bool System::BootWithSeed(uint8_t ipl3_seed, bool version_bit, uint64_t& entry_point)
{
    PrepareIpl3(ipl3_seed, version_bit);

    const uint32_t entry = rom_.size() >= 12 ? xsflib::LoadBe32(rom_.data() + 8) : 0;
    std::vector<uint64_t> saved = breakpoints_;
    breakpoints_.clear();
    for (const Candidate& candidate : kCandidates)
    {
        if (candidate.seed == ipl3_seed)
        {
            AddBreakpoint(SignExtend32(entry - candidate.entry_offset));
        }
    }

    if (ipl3_seed == 0x3F)
    {
        AddBreakpoint(kFixedEntry7102);
    }

    Run(kBootBudgetTicks);
    const bool booted = hit_breakpoint_;
    entry_point = cpu_->Pc();
    breakpoints_ = std::move(saved);
    stop_ = false;

    return booted;
}

BootInfo System::Boot()
{
    BootInfo info;
    if (rom_.size() < 0x1000)
    {
        info.problem = "the ROM is too small to hold a boot program";
        return info;
    }

    for (const Candidate& candidate : kCandidates)
    {
        uint64_t entry = 0;
        if (BootWithSeed(candidate.seed, false, entry))
        {
            info.booted = true;
            info.cic = candidate.cic;
            info.tv = tv_;
            info.entry_point = entry;
            return info;
        }
    }

    // Nothing booted. Leave the state for the most common chip.
    uint64_t entry = 0;
    BootWithSeed(kCandidates[0].seed, false, entry);
    info.tv = tv_;
    info.problem = "IPL3 didn't reach the game's entry point with any boot chip's seed";
    if (pif_rom_.size() < kIpl2End)
    {
        info.problem += ". A game with the CIC-NUS-6105 needs a PIF ROM to boot";
    }

    return info;
}

} // namespace usf::n64
