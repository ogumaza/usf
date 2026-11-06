// SPDX-License-Identifier: MIT

// The PIF's joybus replies for the devices it can connect: the standard controller and the EEPROMs of 4 and 16 Kbit.
// The expected bytes come from n64brew's pages on the joybus protocol (the command table, the identifiers, the
// controller's status byte, the diagram of the state reply, the replies without an accessory and the EEPROM's
// blocks) and on the PIF (the channels and the error flags in the receive-length byte).

#include "n64/pif.h"

#include <array>
#include <cstdint>
#include <vector>

#include "xsflib/testing/check.h"

namespace
{

using usf::n64::EepromType;
using usf::n64::InputEvent;
using usf::n64::Pif;

namespace button = usf::n64::button;

// One channel's command in a joybus frame: the bytes to send and the number of bytes to receive. An empty `send`
// skips the channel with the escape byte 0x00.
struct Command
{
    std::vector<uint8_t> send;
    int receive = 0;
};

// Writes the frame of `commands`, one for each channel from channel 0, ends it with 0xFE and sets the command byte
// to configure. Returns the offset of each command's receive-length byte, or -1 for a skipped channel.
std::vector<int> WriteFrame(Pif& pif, const std::vector<Command>& commands)
{
    auto& ram = pif.Ram();
    ram.fill(0);
    std::vector<int> offsets;
    int offset = 0;
    for (const Command& command : commands)
    {
        if (command.send.empty())
        {
            ram[offset++] = 0x00;
            offsets.push_back(-1);
            continue;
        }

        ram[offset] = static_cast<uint8_t>(command.send.size());
        ram[offset + 1] = static_cast<uint8_t>(command.receive);
        offsets.push_back(offset + 1);
        for (size_t i = 0; i < command.send.size(); i++)
        {
            ram[offset + 2 + static_cast<int>(i)] = command.send[i];
        }
        offset += 2 + static_cast<int>(command.send.size()) + command.receive;
    }

    ram[offset] = 0xFE;
    ram[63] = 0x01;
    pif.OnWrite();

    return offsets;
}

// The reply bytes after the receive-length byte at `offset`.
std::vector<uint8_t> Reply(const Pif& pif, int offset, int send_length, int length)
{
    const auto& ram = pif.Ram();
    const auto start = ram.begin() + offset + 1 + send_length;
    return {start, start + length};
}

void EmptyPorts()
{
    Pif pif;
    pif.Reset();
    const std::vector<int> offsets = WriteFrame(pif, {{{0x00}, 3}, {{0x00}, 3}, {{0x00}, 3}, {{0x00}, 3}, {{0x00}, 3}});

    pif.OnRead(0, 0);

    for (const int offset : offsets)
    {
        XSFLIB_CHECK(pif.Ram()[offset] == (0x80 | 3));
    }
}

void ControllerInfo()
{
    Pif pif;
    pif.Reset();
    pif.ConnectController(1, true);
    const std::vector<int> offsets = WriteFrame(pif, {{}, {{0x00}, 3}, {{0xFF}, 3}});

    pif.OnRead(0, 0);

    // Port 1 answers both info commands with the controller's identifier 0x0500 and "no accessory inserted". Port 2
    // has nothing connected.
    XSFLIB_CHECK(pif.Ram()[offsets[1]] == 3);
    XSFLIB_CHECK(Reply(pif, offsets[1], 1, 3) == std::vector<uint8_t>({0x05, 0x00, 0x02}));
    XSFLIB_CHECK(pif.Ram()[offsets[2]] == (0x80 | 3));
}

void ControllerStateReply()
{
    Pif pif;
    pif.Reset();
    pif.ConnectController(0, true);
    pif.ConnectController(1, true);
    pif.SetController(0, {.buttons = button::kA | button::kCRight, .stick_x = -128, .stick_y = 127});
    pif.SetController(1, {.buttons = button::kStart | button::kL | button::kRight, .stick_x = 5, .stick_y = -1});
    const std::vector<int> offsets = WriteFrame(pif, {{{0x01}, 4}, {{0x01}, 4}});

    pif.OnRead(0, 0);

    // The diagram's order: A, B, Z, Start, up, down, left and right from the top bit of the first byte, then RST, a
    // bit left unused, L, R and the four C buttons. The stick's X and Y follow as two's-complement bytes.
    XSFLIB_CHECK(pif.Ram()[offsets[0]] == 4);
    XSFLIB_CHECK(Reply(pif, offsets[0], 1, 4) == std::vector<uint8_t>({0x80, 0x01, 0x80, 0x7F}));
    XSFLIB_CHECK(Reply(pif, offsets[1], 1, 4) == std::vector<uint8_t>({0x11, 0x20, 0x05, 0xFF}));
}

void ControllerResetCombination()
{
    Pif pif;
    pif.Reset();
    pif.ConnectController(0, true);
    pif.SetController(0, {.buttons = button::kStart | button::kL | button::kR | button::kB});
    const std::vector<int> offsets = WriteFrame(pif, {{{0x01}, 4}});

    pif.OnRead(0, 0);

    // L, R and Start together set RST and clear Start.
    XSFLIB_CHECK(Reply(pif, offsets[0], 1, 2) == std::vector<uint8_t>({0x40, 0xB0}));
}

void NoAccessory()
{
    Pif pif;
    pif.Reset();
    pif.ConnectController(0, true);
    pif.ConnectController(1, true);
    std::vector<uint8_t> write = {0x03, 0x80, 0x01};
    write.resize(3 + 32, 0x00);
    const std::vector<int> offsets = WriteFrame(pif, {{{0x02, 0x80, 0x01}, 33}, {write, 1}});

    pif.OnRead(0, 0);

    // A read gives 32 zeros and the CRC 0xFF. A write's CRC is complemented: the CRC of 32 zeros is 0.
    std::vector<uint8_t> expected(32, 0x00);
    expected.push_back(0xFF);
    XSFLIB_CHECK(pif.Ram()[offsets[0]] == 33);
    XSFLIB_CHECK(Reply(pif, offsets[0], 3, 33) == expected);
    XSFLIB_CHECK(pif.Ram()[offsets[1]] == 1);
    XSFLIB_CHECK(Reply(pif, offsets[1], 35, 1) == std::vector<uint8_t>({0xFF}));
}

void UnsupportedCommandTimesOut()
{
    Pif pif;
    pif.Reset();
    pif.ConnectController(0, true);
    const std::vector<int> offsets = WriteFrame(pif, {{{0x04, 0x00}, 8}});

    pif.OnRead(0, 0);

    XSFLIB_CHECK(pif.Ram()[offsets[0]] == (0x40 | 8));
}

void SkippedChannel()
{
    Pif pif;
    pif.Reset();
    pif.ConnectController(0, true);
    WriteFrame(pif, {{{0x00}, 3}});
    pif.Ram()[0] = 0x81; // The skip flag on a send length of 1.

    pif.OnRead(0, 0);

    XSFLIB_CHECK(pif.Ram()[1] == 3);
    XSFLIB_CHECK(Reply(pif, 1, 1, 3) == std::vector<uint8_t>({0x00, 0x00, 0x00}));
}

void Eeprom(EepromType type)
{
    Pif pif;
    pif.Reset();
    pif.SetEeprom(type);

    const std::vector<Command> write = {{}, {}, {}, {}, {{0x05, 3, 1, 2, 3, 4, 5, 6, 7, 8}, 1}};
    const std::vector<int> write_offsets = WriteFrame(pif, write);
    pif.OnRead(0, 0);
    const uint8_t write_status = pif.Ram()[write_offsets[4]];
    const std::vector<uint8_t> write_reply = Reply(pif, write_offsets[4], 10, 1);

    const std::vector<int> offsets = WriteFrame(pif, {{}, {}, {}, {}, {{0x00}, 3}});
    pif.OnRead(0, 0);
    const std::vector<uint8_t> info = Reply(pif, offsets[4], 1, 3);

    const std::vector<int> read_offsets = WriteFrame(pif, {{}, {}, {}, {}, {{0x04, 3}, 8}});
    pif.OnRead(0, 0);
    const std::vector<uint8_t> block = Reply(pif, read_offsets[4], 2, 8);
    WriteFrame(pif, {{}, {}, {}, {}, {{0x04, 67}, 8}});
    pif.OnRead(0, 0);
    const std::vector<uint8_t> wrapped = Reply(pif, read_offsets[4], 2, 8);

    // The identifier is 0x0080 for 4 Kbit and 0x00C0 for 16 Kbit. A write answers 0x00 when it starts. The 4 Kbit part
    // ignores the top two bits of the block number, so block 67 is block 3.
    const bool small = type == EepromType::k4Kbit;
    XSFLIB_CHECK(write_status == 1);
    XSFLIB_CHECK(write_reply == std::vector<uint8_t>({0x00}));
    XSFLIB_CHECK(info == std::vector<uint8_t>({0x00, static_cast<uint8_t>(small ? 0x80 : 0xC0), 0x00}));
    XSFLIB_CHECK(block == std::vector<uint8_t>({1, 2, 3, 4, 5, 6, 7, 8}));
    XSFLIB_CHECK((wrapped == block) == small);
    XSFLIB_CHECK(pif.EepromData().size() == (small ? 512u : 2048u));
}

void NoEeprom()
{
    Pif pif;
    pif.Reset();
    const std::vector<int> offsets = WriteFrame(pif, {{}, {}, {}, {}, {{0x00}, 3}});

    pif.OnRead(0, 0);

    XSFLIB_CHECK(pif.Ram()[offsets[4]] == (0x80 | 3));
}

void InputScript()
{
    Pif pif;
    pif.Reset();
    pif.ConnectController(0, true);
    pif.AddInput({.clock = InputEvent::Clock::kFields, .at = 10, .port = 0, .state = {.buttons = button::kStart}});
    pif.AddInput({.clock = InputEvent::Clock::kTicks, .at = 1000, .port = 0, .state = {.buttons = button::kA}});

    const auto read = [&pif](uint64_t ticks, uint64_t fields)
    {
        const std::vector<int> offsets = WriteFrame(pif, {{{0x01}, 4}});
        pif.OnRead(ticks, fields);

        return Reply(pif, offsets[0], 1, 2);
    };

    const std::vector<uint8_t> before = read(5000, 9);
    const std::vector<uint8_t> both = read(5000, 10);
    const std::vector<uint8_t> a = read(5000, 11);
    pif.Reset();
    const std::vector<uint8_t> again = read(0, 10);

    // The tick event's time passed before the field event's, but it waits for the event added ahead of it. A reset
    // starts the script again.
    XSFLIB_CHECK(before == std::vector<uint8_t>({0x00, 0x00}));
    XSFLIB_CHECK(both == std::vector<uint8_t>({0x80, 0x00}));
    XSFLIB_CHECK(a == std::vector<uint8_t>({0x80, 0x00}));
    XSFLIB_CHECK(again == std::vector<uint8_t>({0x10, 0x00}));
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("pif",
                                    []
                                    {
                                        EmptyPorts();
                                        ControllerInfo();
                                        ControllerStateReply();
                                        ControllerResetCombination();
                                        NoAccessory();
                                        UnsupportedCommandTimesOut();
                                        SkippedChannel();
                                        Eeprom(EepromType::k4Kbit);
                                        Eeprom(EepromType::k16Kbit);
                                        NoEeprom();
                                        InputScript();
                                    });
}
