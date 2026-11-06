// SPDX-License-Identifier: MIT

// Short programs on the CPU, with results that the VR4300's manual defines: sign extension of 32-bit results,
// unaligned loads and stores, delay slots and the exception vector.

#include <cstdint>
#include <initializer_list>

#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

#include "n64/system.h"

namespace
{

using usf::n64::System;

constexpr uint32_t kCode = 0x1000;
constexpr uint64_t kCodeAddress = 0xFFFFFFFF80000000ull + kCode;

// Puts the program at kCode and runs it for `instructions` instructions.
void RunProgram(System& system, std::initializer_list<uint32_t> program, uint64_t instructions)
{
    uint32_t address = kCode;
    for (uint32_t word : program)
    {
        xsflib::StoreBe32(system.Rdram().data() + address, word);
        address += 4;
    }

    system.GetCpu().SetPc(kCodeAddress);
    system.Run(instructions * system.TicksPerInstruction());
}

void SignExtension()
{
    System system;

    // lui t0, 0x7FFF; ori t0, t0, 0xFFFF; addiu t1, t0, 1
    RunProgram(system, {0x3C087FFF, 0x3508FFFF, 0x25090001}, 3);

    XSFLIB_CHECK(system.GetCpu().Gpr(8) == 0x7FFFFFFF);
    XSFLIB_CHECK(system.GetCpu().Gpr(9) == 0xFFFFFFFF80000000ull);
}

void UnalignedLoads()
{
    System system;
    xsflib::StoreBe32(system.Rdram().data() + 0x2000, 0x11223344);
    xsflib::StoreBe32(system.Rdram().data() + 0x2004, 0x55667788);

    // lui t2, 0x8000; lwl t0, 0x2001(t2); lwr t0, 0x2004(t2)
    RunProgram(system, {0x3C0A8000, 0x89482001, 0x99482004}, 3);

    XSFLIB_CHECK(system.GetCpu().Gpr(8) == 0x22334455);
}

void UnalignedStores()
{
    System system;

    // lui t2, 0x8000; lui t0, 0xAABB; ori t0, t0, 0xCCDD; swl t0, 0x2001(t2); swr t0, 0x2004(t2)
    RunProgram(system, {0x3C0A8000, 0x3C08AABB, 0x3508CCDD, 0xA9482001, 0xB9482004}, 5);

    XSFLIB_CHECK(xsflib::LoadBe32(system.Rdram().data() + 0x2000) == 0x00AABBCC);
    XSFLIB_CHECK(xsflib::LoadBe32(system.Rdram().data() + 0x2004) == 0xDD000000);
}

void DelaySlot()
{
    System system;

    // beq zero, zero, +2; addiu t0, zero, 1; addiu t1, zero, 2; addiu t2, zero, 3
    RunProgram(system, {0x10000002, 0x24080001, 0x24090002, 0x240A0003}, 3);

    XSFLIB_CHECK(system.GetCpu().Gpr(8) == 1);
    XSFLIB_CHECK(system.GetCpu().Gpr(9) == 0);
    XSFLIB_CHECK(system.GetCpu().Gpr(10) == 3);
}

void Syscall()
{
    System system;

    // nop; syscall
    RunProgram(system, {0x00000000, 0x0000000C}, 2);

    const usf::n64::Cpu& cpu = system.GetCpu();
    XSFLIB_CHECK(cpu.Pc() == 0xFFFFFFFF80000180ull);
    XSFLIB_CHECK(cpu.Cop0(usf::n64::kCop0Epc) == kCodeAddress + 4);
    XSFLIB_CHECK(((cpu.Cop0(usf::n64::kCop0Cause) >> 2) & 0x1F) == 8);
    XSFLIB_CHECK((cpu.Cop0(usf::n64::kCop0Status) & usf::n64::kStatusExl) != 0);
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("cpu",
                                    []
                                    {
                                        SignExtension();
                                        UnalignedLoads();
                                        UnalignedStores();
                                        DelaySlot();
                                        Syscall();
                                    });
}
