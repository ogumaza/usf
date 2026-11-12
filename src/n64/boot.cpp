// SPDX-License-Identifier: MIT

// Boot. The PIF's boot code (IPL1 and IPL2) isn't run: the system starts in the state it leaves for the cartridge's
// IPL3, which then runs as on the hardware. The facts come from the n64brew wiki's pages on the PIF and on the initial
// program load.

#include <algorithm>
#include <array>
#include <utility>
#include <vector>

#include "xsflib/bytes.h"

#include "n64/bits.h"
#include "n64/mips_registers.h"
#include "n64/system.h"

namespace usf::n64
{
namespace
{

// IPL2 occupies the PIF ROM from offset 0xD4 to 0x71B. IPL1 copies it to the start of IMEM.
constexpr uint32_t kIpl2Start = 0xD4;
constexpr uint32_t kIpl2End = 0x71C;

// IPL3 follows the cartridge's header, at offset 0x40, up to the end of the first 4 KiB. IPL2 copies it to DMEM at the
// same offset.
constexpr uint32_t kIpl3Start = 0x40;

// Where IPL2 starts IPL3, in DMEM through KSEG1, and the stack that it leaves in SP: 16 bytes below the end of IMEM.
constexpr uint32_t kIpl3Entry = kKseg1 + kSpDmem + kIpl3Start;
constexpr uint32_t kIpl2Stack = kKseg1 + kSpImem + kSpMemSize - 0x10;

// The region's version bits in S7. An NTSC console has none.
constexpr uint32_t kVersionBitsPal = 6;
constexpr uint32_t kVersionBitsMpal = 4;

// The return address that IPL2 leaves in RA, in IMEM. It differs on an NTSC console.
constexpr uint32_t kIpl2ReturnNtsc = 0xA4001550;
constexpr uint32_t kIpl2ReturnOther = 0xA4001554;

// The CIC-NUS-6102's seed. The 6101 and the 7102 share it.
constexpr uint8_t kSeed6102 = 0x3F;

struct Candidate
{
    Cic cic;
    uint8_t seed;
    uint32_t entry_offset; // IPL3 subtracts this from the header's entry point.
};

// The seeds of the boot chips. The 6101 shares the 6102's seed. The 7102 jumps to a fixed address. The CIC-NUS-5101 is
// the boot chip of Seta's Aleck64 arcade board.
constexpr std::array<Candidate, 5> kCandidates = {{
    {Cic::k6102, kSeed6102, 0},
    {Cic::k6103, 0x78, 0x100000},
    {Cic::k6105, 0x91, 0},
    {Cic::k6106, 0x85, 0x200000},
    {Cic::k5101, 0xAC, 0x100000},
}};

constexpr uint64_t kFixedEntry7102 = 0xFFFFFFFF80000480ull;

// The most that IPL3 runs for before it gives up. A cold boot takes a fraction of a second.
constexpr uint64_t kBootBudgetTicks = 4 * kCpuClock;

} // namespace

void System::PrepareIpl3(uint8_t ipl3_seed)
{
    Reset();
    ResetRdramChips(false);

    // IPL2 copies IPL3 from the cartridge into DMEM, and leaves itself in IMEM.
    auto& dmem = rsp_->Dmem();
    for (uint32_t i = kIpl3Start; i < kSpMemSize; i++)
    {
        dmem[i] = i < rom_.size() ? rom_[i] : 0;
    }

    auto& imem = rsp_->Imem();
    if (pif_rom_.size() >= kIpl2End)
    {
        std::copy(pif_rom_.begin() + kIpl2Start, pif_rom_.begin() + kIpl2End, imem.begin());
    }

    // The PI's timing for domain 1 comes from the header's first word. The tests call this function with ROMs shorter
    // than a header.
    const uint32_t header = rom_.size() >= 4 ? xsflib::LoadBe32(rom_.data()) : 0;
    pi_.domains[0] = header & 0xFF;
    pi_.domains[1] = (header >> 8) & 0xFF;
    pi_.domains[2] = (header >> 16) & 0x0F;
    pi_.domains[3] = (header >> 20) & 0x03;

    // The half-line of the VI interrupt that IPL2 leaves in VI_INTR.
    vi_.registers[Vi::kIntr] = 0x3FF;
    ScheduleVi();

    // The registers that IPL3 reads: the television system, the reset type, the seed and the region's version bits.
    const uint32_t region = tv_ == TvType::kPal ? kVersionBitsPal : (tv_ == TvType::kMpal ? kVersionBitsMpal : 0);
    cpu_->SetGpr(mips::kS3, 0);
    cpu_->SetGpr(mips::kS4, static_cast<uint32_t>(tv_));
    cpu_->SetGpr(mips::kS5, 0);
    cpu_->SetGpr(mips::kS6, ipl3_seed);
    cpu_->SetGpr(mips::kS7, region);
    cpu_->SetGpr(mips::kSp, SignExtend32(kIpl2Stack));
    cpu_->SetGpr(mips::kT3, SignExtend32(kIpl3Entry));
    cpu_->SetGpr(mips::kRa, SignExtend32(tv_ == TvType::kNtsc ? kIpl2ReturnNtsc : kIpl2ReturnOther));
    cpu_->SetPc(SignExtend32(kIpl3Entry));
}

bool System::BootWithSeed(uint8_t ipl3_seed, uint64_t& entry_point)
{
    PrepareIpl3(ipl3_seed);

    // Boot has checked that the ROM holds the header.
    const uint32_t entry = xsflib::LoadBe32(rom_.data() + kHeaderEntryPoint);
    std::vector<uint64_t> saved = std::exchange(breakpoints_, {});
    for (const Candidate& candidate : kCandidates)
    {
        if (candidate.seed == ipl3_seed)
        {
            AddBreakpoint(SignExtend32(entry - candidate.entry_offset));
        }
    }

    if (ipl3_seed == kSeed6102)
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
    if (rom_.size() < kSpMemSize)
    {
        info.problem = "the ROM is too small to hold a boot program";
        return info;
    }

    for (const Candidate& candidate : kCandidates)
    {
        uint64_t entry = 0;
        if (BootWithSeed(candidate.seed, entry))
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
    BootWithSeed(kCandidates[0].seed, entry);
    info.tv = tv_;
    info.problem = "IPL3 didn't reach the game's entry point with any boot chip's seed";
    if (pif_rom_.size() < kIpl2End)
    {
        info.problem += ". A game with the CIC-NUS-6105 needs a PIF ROM to boot";
    }

    return info;
}

} // namespace usf::n64
