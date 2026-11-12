// SPDX-License-Identifier: MIT

// What the small test programs share: a ROM, programs written into memory, a CPU about to run one, the options of a
// set, and the RSP's vector instructions. test_mips.h has the helpers that use the ripper's MIPS encoder.

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "xsflib/bytes.h"

#include "n64/bits.h"
#include "n64/constants.h"
#include "n64/cpu.h"
#include "n64/save_state.h"
#include "n64/system.h"

namespace usf::testing
{

// The start of KSEG0 as a CPU register holds it, sign-extended to 64 bits.
inline constexpr uint64_t kKseg0Address = n64::SignExtend32(n64::kKseg0);

// Status with COP0 and COP1 usable.
inline constexpr uint32_t kStatusCu0And1 = n64::kStatusCu0 | n64::kStatusCu1;

// Where the test programs start: a physical address, and the same address in KSEG0.
inline constexpr uint32_t kCode = 0x00001000;
inline constexpr uint64_t kCodeAddress = kKseg0Address + kCode;

// The position of register a0 in a state image. a0 is register 4.
inline constexpr uint32_t kStateA0 = n64::kStateGprOffset + 8 * 4;

// A ROM of `size` bytes in the console byte order: the first word of a cartridge header, then zeros.
inline std::vector<uint8_t> TestRom(size_t size)
{
    std::vector<uint8_t> rom(size, 0);
    xsflib::StoreBe32(rom.data(), 0x80371240);

    return rom;
}

// Writes `words` to the physical addresses from `address` on.
inline void WriteProgram(n64::System& system, uint32_t address, const std::vector<uint32_t>& words)
{
    for (size_t i = 0; i < words.size(); i++)
    {
        system.WritePhysical32(address + 4 * static_cast<uint32_t>(i), words[i]);
    }
}

// Puts `words` at kCode, sets Status in COP0 to `status` and moves the CPU to the first word.
inline void StartProgram(n64::System& system, const std::vector<uint32_t>& words, uint32_t status)
{
    WriteProgram(system, kCode, words);
    system.GetCpu().SetCop0(n64::kCop0Status, status);
    system.GetCpu().SetPc(kCodeAddress);
}

// Puts `words` at kCode and runs them for `instructions` instructions.
inline void RunProgram(n64::System& system, const std::vector<uint32_t>& words, uint64_t instructions)
{
    WriteProgram(system, kCode, words);
    system.GetCpu().SetPc(kCodeAddress);
    system.Run(instructions * system.TicksPerInstruction());
}

// The options of a USF set: the timing of the legacy players.
inline n64::Options LegacyOptions()
{
    n64::Options options;
    options.timing = n64::Timing::kLegacy;

    return options;
}

// The save state of a system with the legacy timing and `rom`, about to run `words` from kCode with `status` in
// Status.
inline std::vector<uint8_t> ProgramState(const std::vector<uint8_t>& rom, const std::vector<uint32_t>& words,
                                         uint32_t status)
{
    n64::System system(LegacyOptions());
    system.LoadRom(rom);
    StartProgram(system, words, status);

    return system.SaveState();
}

// The options of a set with both flags off, with the timer interrupt on or off.
inline n64::Options SetOptions(bool timer_interrupt)
{
    n64::Options options = LegacyOptions();
    options.timer_interrupt = timer_interrupt;
    options.ai_delay = false;
    options.rcp_delays = false;
    options.fpu_traps = false;
    options.exact_events = false;

    return options;
}

// The function codes of the RSP's vector instructions.
inline constexpr uint32_t kVmulf = 0x00;
inline constexpr uint32_t kVmudh = 0x07;
inline constexpr uint32_t kVmacf = 0x08;
inline constexpr uint32_t kVmadh = 0x0F;
inline constexpr uint32_t kVadd = 0x10;
inline constexpr uint32_t kVsub = 0x11;
inline constexpr uint32_t kVsar = 0x1D;

// A vector instruction of the RSP.
inline uint32_t RspVector(uint32_t funct, uint32_t vd, uint32_t vs, uint32_t vt, uint32_t element = 0)
{
    return 0x4A000000 | (element << 21) | (vt << 16) | (vs << 11) | (vd << 6) | funct;
}

// A load or store of a whole vector at `sixteens` times 16 bytes from address 0 of DMEM.
inline uint32_t RspLqv(uint32_t vt, uint32_t sixteens)
{
    return 0xC8000000 | (vt << 16) | (4u << 11) | sixteens;
}

inline uint32_t RspSqv(uint32_t vt, uint32_t sixteens)
{
    return 0xE8000000 | (vt << 16) | (4u << 11) | sixteens;
}

} // namespace usf::testing
