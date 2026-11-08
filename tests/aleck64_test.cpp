// SPDX-License-Identifier: MIT

// The Aleck64's board as the CPU's bus sees it: its RAM at 0xC0000000, its inputs at 0xC0800000 with the mahjong
// panel's rows, the E90's video memory, the console's view of the same addresses, and the legacy page tables' view of
// TLB entries that map the board's RAM. The addresses and bit meanings
// come from the board's behavioural specification.

#include <cstdint>

#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

#include "n64/system.h"

namespace
{

using usf::n64::Board;
using usf::n64::BoardInputEvent;
using usf::n64::BoardInputs;
using usf::n64::Options;
using usf::n64::System;

Options BoardOptions(Board board)
{
    Options options;
    options.board = board;
    options.rdram_size = usf::n64::kRdramSize8;

    return options;
}

void Ram()
{
    System system(BoardOptions(Board::kAleck64));

    system.WritePhysical32(0xC0000000, 0x11223344);
    system.WritePhysical32(0xC07FFFFC, 0x55667788);
    system.WriteBus(0xC0000005, 0xAB, 1);
    system.WriteBus(0xC0000010, 0x0102030405060708ull, 8);

    XSFLIB_CHECK(system.ReadPhysical32(0xC0000000) == 0x11223344);
    XSFLIB_CHECK(system.ReadPhysical32(0xC07FFFFC) == 0x55667788);
    XSFLIB_CHECK(system.ReadPhysical32(0xC0000004) == 0x00AB0000);
    XSFLIB_CHECK(system.ReadBus64(0xC0000010) == 0x0102030405060708ull);
    XSFLIB_CHECK(system.BoardAccesses() == 9);
}

void Inputs()
{
    System system(BoardOptions(Board::kAleck64));
    BoardInputs inputs;
    inputs.switches = 0xFFDFFFFF;
    inputs.system = 0xFFFBFFFF;
    system.SetBoardInputs(inputs);

    XSFLIB_CHECK(system.ReadPhysical32(0xC0800000) == 0xFFDFFFFF);
    XSFLIB_CHECK(system.ReadPhysical32(0xC0800004) == 0xFFFBFFFF);
    XSFLIB_CHECK(system.ReadPhysical32(0xC0800008) == 0);
    XSFLIB_CHECK(system.ReadPhysical32(0xC0800100) == 0);
    XSFLIB_CHECK(system.ReadPhysical32(0xC0800FFC) == 0);
}

void Panel()
{
    System system(BoardOptions(Board::kAleck64));
    BoardInputs inputs;
    inputs.panel = true;
    inputs.rows = {0xFE, 0xFD, 0xFB, 0xF7};
    system.SetBoardInputs(inputs);

    system.WritePhysical32(0xC0800008, 0x00000400);
    const uint32_t third = system.ReadPhysical32(0xC0800008);
    system.WritePhysical32(0xC0800008, 0x00000100);
    const uint32_t first = system.ReadPhysical32(0xC0800008);
    system.WritePhysical32(0xC0800008, 0x00000300);
    const uint32_t two_rows = system.ReadPhysical32(0xC0800008);

    XSFLIB_CHECK(third == 0xFFFBFFFF);
    XSFLIB_CHECK(first == 0xFFFEFFFF);
    XSFLIB_CHECK(two_rows == 0);
}

void InputScript()
{
    System system(BoardOptions(Board::kAleck64));
    BoardInputEvent coin;
    coin.field = 1;
    coin.inputs.system = 0xFFFBFFFF;
    system.AddBoardInput(coin);

    system.PrepareIpl3(0xAC, false);
    const uint32_t before = system.ReadPhysical32(0xC0800004);
    system.Run(system.TicksPerSecond() / 20);
    const uint32_t after = system.ReadPhysical32(0xC0800004);

    XSFLIB_CHECK(before == 0xFFFFFFFF);
    XSFLIB_CHECK(system.Fields() >= 1);
    XSFLIB_CHECK(after == 0xFFFBFFFF);
}

void VideoChip()
{
    System e90(BoardOptions(Board::kAleck64E90));
    System e92(BoardOptions(Board::kAleck64));

    e90.WritePhysical32(0xD0000FFC, 0x12345678);
    e90.WritePhysical32(0xD0010000, 0x9ABCDEF0);
    e92.WritePhysical32(0xD0000000, 0x12345678);

    XSFLIB_CHECK(e90.ReadPhysical32(0xD0000FFC) == 0x12345678);
    XSFLIB_CHECK(e90.ReadPhysical32(0xD0010000) == 0x9ABCDEF0);
    XSFLIB_CHECK(e90.ReadPhysical32(0xD0030000) == 0);
    XSFLIB_CHECK(e92.ReadPhysical32(0xD0000000) == 0);
}

// A console has nothing above 0x80000000. The bus holds the low half of the address, and the core counts the access.
void Console()
{
    System system;

    const uint32_t value = system.ReadPhysical32(0xC0800004);

    XSFLIB_CHECK(value == 0x00040004);
    XSFLIB_CHECK(system.BoardAccesses() == 1);
}

// A program that loads a word through a TLB entry that maps 0xE0000000 to the board's RAM, as the board's games map
// their heap, and returns the loaded word.
uint32_t LoadThroughTlb(System& system)
{
    usf::n64::TlbEntry entry;
    entry.entry_hi = 0xFFFFFFFFE0000000ull;
    entry.entry_lo0 = ((0xC0000000u >> 12) << 6) | 7; // Dirty, valid and global.
    entry.entry_lo1 = ((0xC0001000u >> 12) << 6) | 7;
    entry.global = true;
    system.GetCpu().SetTlb(0, entry);

    constexpr uint32_t kCode = 0x1000;
    constexpr uint32_t kLuiT0 = 0x3C08E000;  // lui t0, 0xE000
    constexpr uint32_t kLoadT3 = 0x8D0B0004; // lw t3, 4(t0)
    xsflib::StoreBe32(system.Rdram().data() + kCode, kLuiT0);
    xsflib::StoreBe32(system.Rdram().data() + kCode + 4, kLoadT3);
    system.GetCpu().SetPc(0xFFFFFFFF80000000ull + kCode);
    system.Run(2 * system.TicksPerInstruction());

    return static_cast<uint32_t>(system.GetCpu().Gpr(11));
}

// The legacy players refuse a TLB entry that maps a frame past the console's physical addresses. The board's run maps
// it. A console's run counts the access that faults.
void LegacyPages()
{
    Options board_options = BoardOptions(Board::kAleck64);
    board_options.timing = usf::n64::Timing::kLegacy;
    System board(board_options);
    board.WritePhysical32(0xC0000004, 0x12345678);
    const uint64_t board_before = board.BoardAccesses();
    const uint32_t board_word = LoadThroughTlb(board);

    Options console_options;
    console_options.timing = usf::n64::Timing::kLegacy;
    System console(console_options);
    const uint32_t console_word = LoadThroughTlb(console);

    XSFLIB_CHECK(board_word == 0x12345678);
    XSFLIB_CHECK(board.BoardAccesses() == board_before + 1);
    XSFLIB_CHECK(console_word == 0);
    XSFLIB_CHECK(console.BoardAccesses() == 1);
}

void ResetKeepsInputs()
{
    System system(BoardOptions(Board::kAleck64));
    BoardInputs inputs;
    inputs.switches = 0x7FFFFFFF;
    system.SetBoardInputs(inputs);
    system.WritePhysical32(0xC0000000, 1);

    system.PrepareIpl3(0xAC, false);

    XSFLIB_CHECK(system.ReadPhysical32(0xC0800000) == 0x7FFFFFFF);
    XSFLIB_CHECK(system.ReadPhysical32(0xC0000000) == 0);
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("aleck64",
                                    []
                                    {
                                        Ram();
                                        Inputs();
                                        Panel();
                                        InputScript();
                                        VideoChip();
                                        Console();
                                        LegacyPages();
                                        ResetKeepsInputs();
                                    });
}
