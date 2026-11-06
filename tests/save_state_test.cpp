// SPDX-License-Identifier: MIT

// The Project64 1.4 save state: its layout, and a round trip through the reader and the writer.

#include <cstdint>
#include <vector>

#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

#include "n64/system.h"

namespace
{

using usf::n64::Cpu;
using usf::n64::System;

std::vector<uint8_t> TestRom()
{
    // A cartridge of 1 MiB with a PAL country code in its header.
    std::vector<uint8_t> rom(0x100000, 0);
    xsflib::StoreBe32(rom.data(), 0x80371240);
    rom[0x3E] = 0x50;

    return rom;
}

void Layout()
{
    System system;
    system.LoadRom(TestRom());
    Cpu& cpu = system.GetCpu();
    cpu.SetPc(0xFFFFFFFF80001234ull);
    cpu.SetGpr(3, 0x1122334455667788ull);
    cpu.SetHi(0x0102030405060708ull);
    system.Rdram()[0x1000] = 0xAB;
    system.WritePhysical32(0x0430000C, 0x2); // MI_MASK: set the SP interrupt's mask bit.
    system.GetPif().Ram()[1] = 0x84;

    const std::vector<uint8_t> state = system.SaveState();

    XSFLIB_CHECK(state.size() == 0x40275C);
    XSFLIB_CHECK(xsflib::LoadLe32(state.data()) == 0x23D8A6C8);
    XSFLIB_CHECK(xsflib::LoadLe32(state.data() + 0x004) == 0x400000);
    XSFLIB_CHECK(state[0x008 + (0x3E ^ 3)] == 0x50);
    XSFLIB_CHECK(xsflib::LoadLe32(state.data() + 0x04C) == 0x80001234);
    XSFLIB_CHECK(xsflib::LoadLe64(state.data() + 0x068) == 0x1122334455667788ull);
    XSFLIB_CHECK(xsflib::LoadLe64(state.data() + 0x350) == 0x0102030405060708ull);
    XSFLIB_CHECK(xsflib::LoadLe32(state.data() + 0x3DC) == 0x02020102);
    XSFLIB_CHECK(xsflib::LoadLe32(state.data() + 0x3E4) == 0x1);
    XSFLIB_CHECK(state[0x75C + (0x1000 ^ 3)] == 0xAB);
    XSFLIB_CHECK(state[0x71C + 1] == 0x84); // PIF RAM is in console byte order.
}

void RoundTrip(uint32_t status)
{
    System system;
    system.LoadRom(TestRom());
    Cpu& cpu = system.GetCpu();
    cpu.SetCop0(usf::n64::kCop0Status, status);

    for (int i = 0; i < 32; i++)
    {
        cpu.SetGpr(i, 0x0101010101010101ull * static_cast<uint64_t>(i));
        cpu.SetFpr(i, 0x1000000000000001ull * static_cast<uint64_t>(i + 1));
    }

    cpu.SetCop0(usf::n64::kCop0Epc, 0xFFFFFFFF80000400ull);
    cpu.SetCop0(usf::n64::kCop0Compare, 0x12345678);
    cpu.SetFcr31(0x01000800);
    system.Rdram()[0x200] = 0x5A;

    const std::vector<uint8_t> state = system.SaveState();
    System loaded;
    loaded.LoadRom(TestRom());
    loaded.LoadState(state);

    XSFLIB_CHECK(loaded.SaveState() == state);
    XSFLIB_CHECK(loaded.GetCpu().Gpr(7) == 0x0707070707070707ull);
    XSFLIB_CHECK(loaded.GetCpu().Cop0(usf::n64::kCop0Epc) == 0xFFFFFFFF80000400ull);
    XSFLIB_CHECK(loaded.GetCpu().Fcr31() == 0x01000800);
    XSFLIB_CHECK(loaded.Rdram()[0x200] == 0x5A);

    // With FR set, slot n holds register n. With FR clear, slot k holds even register 2k, and the odd registers aren't
    // kept.
    const bool fr = (status & usf::n64::kStatusFr) != 0;
    XSFLIB_CHECK(xsflib::LoadLe64(state.data() + 0x150 + 8 * 2) == (fr ? 3 : 5) * 0x1000000000000001ull);
    XSFLIB_CHECK(loaded.GetCpu().Fpr(4) == 5 * 0x1000000000000001ull);
    XSFLIB_CHECK(loaded.GetCpu().Fpr(5) == (fr ? 6 * 0x1000000000000001ull : 0));
    if (!fr)
    {
        XSFLIB_CHECK(xsflib::LoadLe64(state.data() + 0x150 + 8 * 16) == 0);
    }
}

// A state's joybus frame stays in force. A controller command in it comes back with the "no device" bit (0x80) in its
// receive-length byte, at the first SI read after the load. The state's receive-length byte holds the 4 of the frame,
// and the command byte's configure bit is clear.
void JoybusFrameOfAState()
{
    constexpr uint32_t kSiDramAddress = 0x04800000;
    constexpr uint32_t kSiReadPif = 0x04800004;
    constexpr uint32_t kReply = 0x1000;
    System system;
    system.LoadRom(TestRom());
    auto& ram = system.GetPif().Ram();
    ram[0] = 0x01; // One byte to send.
    ram[1] = 0x04; // Four to receive.
    ram[2] = 0x01; // The state command.
    ram[7] = 0xFE; // The end of the frame.
    const std::vector<uint8_t> state = system.SaveState();
    System loaded;
    loaded.LoadRom(TestRom());

    loaded.LoadState(state);
    const uint8_t before_read = loaded.GetPif().Ram()[1];
    loaded.WritePhysical32(kSiDramAddress, kReply);
    loaded.WritePhysical32(kSiReadPif, 0x1FC007C0);

    XSFLIB_CHECK(before_read == 0x04);
    XSFLIB_CHECK(loaded.GetPif().Ram()[1] == 0x84);
    XSFLIB_CHECK(loaded.Rdram()[kReply + 1] == 0x84);
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("save_state",
                                    []
                                    {
                                        Layout();
                                        RoundTrip(0x34000000); // FR set.
                                        RoundTrip(0x30000000); // FR clear.
                                        JoybusFrameOfAState();
                                    });
}
