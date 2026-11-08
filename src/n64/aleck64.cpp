// SPDX-License-Identifier: MIT

// Seta's Aleck64 arcade board: the memory and inputs that it adds above 0x80000000 physical, where a console has
// nothing. The games map these areas through the TLB.
//
// - 8 MiB of RAM at 0xC0000000. The board holds 4 MiB of SDRAM. No game is known to use more.
// - The inputs at 0xC0800000: the DIP switches and JAMMA controls, then the coins, service, test and starts, then a
//   mahjong panel that the game reads a row at a time. The other words of the block read 0, and writes there are lost.
// - On the E90 main board, the video chip's list of tiles at 0xD0000000 and its palette at 0xD0010000. Its registers,
//   at 0xD0030000, read 0.
//
// Every other address from 0x80000000 up reads 0, and writes there are lost.

#include "xsflib/bytes.h"

#include "n64/system.h"

namespace usf::n64
{

namespace
{

// Where `paddr` falls in a block of `size` bytes at `base`, or `size` when it falls outside.
uint32_t OffsetIn(uint32_t paddr, uint32_t base, uint32_t size)
{
    return paddr >= base && paddr - base < size ? paddr - base : size;
}

void StoreBytes(std::vector<uint8_t>& memory, uint32_t offset, uint64_t value, int size)
{
    if (offset + static_cast<uint32_t>(size) > memory.size())
    {
        return;
    }

    for (int i = size - 1; i >= 0; i--)
    {
        memory[offset + static_cast<uint32_t>(i)] = static_cast<uint8_t>(value);
        value >>= 8;
    }
}

} // namespace

void System::SetBoardInputs(const BoardInputs& inputs)
{
    board_.initial_inputs = inputs;
    board_.inputs = inputs;
}

void System::AddBoardInput(const BoardInputEvent& event)
{
    board_.script.push_back(event);
}

uint64_t System::BoardAccesses() const
{
    return board_accesses_ + cpu_->RefusedBoardPages();
}

void System::ResetBoard()
{
    board_accesses_ = 0;
    board_.inputs = board_.initial_inputs;
    board_.next_input = 0;
    board_.panel_select = 0;
    if (options_.board == Board::kConsole)
    {
        return;
    }

    board_.ram.assign(kAleck64RamSize, 0);
    if (options_.board == Board::kAleck64E90)
    {
        board_.tiles.assign(kAleck64VideoMemorySize, 0);
        board_.palette.assign(kAleck64VideoMemorySize, 0);
    }
}

uint32_t System::ReadBoard32(uint32_t paddr)
{
    paddr &= ~3u;
    if (const uint32_t offset = OffsetIn(paddr, kAleck64Ram, kAleck64RamSize); offset < kAleck64RamSize)
    {
        return xsflib::LoadBe32(board_.ram.data() + offset);
    }
    if (OffsetIn(paddr, kAleck64Inputs, kAleck64InputsSize) < kAleck64InputsSize)
    {
        return ReadBoardInputs(paddr);
    }
    if (options_.board == Board::kAleck64E90)
    {
        if (const uint32_t offset = OffsetIn(paddr, kAleck64Tiles, kAleck64VideoMemorySize);
            offset < kAleck64VideoMemorySize)
        {
            return xsflib::LoadBe32(board_.tiles.data() + offset);
        }
        if (const uint32_t offset = OffsetIn(paddr, kAleck64Palette, kAleck64VideoMemorySize);
            offset < kAleck64VideoMemorySize)
        {
            return xsflib::LoadBe32(board_.palette.data() + offset);
        }
    }

    return 0;
}

uint32_t System::ReadBoardInputs(uint32_t paddr)
{
    while (board_.next_input < board_.script.size() && board_.script[board_.next_input].field <= vi_.fields)
    {
        board_.inputs = board_.script[board_.next_input].inputs;
        board_.next_input++;
    }

    const BoardInputs& inputs = board_.inputs;
    switch (paddr)
    {
    case kAleck64Switches:
        return inputs.switches;
    case kAleck64System:
        return inputs.system;
    case kAleck64Panel:
        if (inputs.panel)
        {
            for (size_t row = 0; row < inputs.rows.size(); row++)
            {
                if (board_.panel_select == 1u << row)
                {
                    return 0xFF00FFFFu | (uint32_t{inputs.rows[row]} << 16);
                }
            }
        }

        return 0;
    default:
        return 0;
    }
}

void System::WriteBoard(uint32_t paddr, uint64_t value, int size)
{
    if (const uint32_t offset = OffsetIn(paddr, kAleck64Ram, kAleck64RamSize); offset < kAleck64RamSize)
    {
        StoreBytes(board_.ram, offset, value, size);
        return;
    }
    if (OffsetIn(paddr, kAleck64Inputs, kAleck64InputsSize) < kAleck64InputsSize)
    {
        // Only the panel's row select takes a write. The game writes it as a word.
        if ((paddr & ~3u) == kAleck64Panel && size == 4)
        {
            board_.panel_select = (static_cast<uint32_t>(value) >> 8) & 0xFF;
        }

        return;
    }
    if (options_.board == Board::kAleck64E90)
    {
        if (const uint32_t offset = OffsetIn(paddr, kAleck64Tiles, kAleck64VideoMemorySize);
            offset < kAleck64VideoMemorySize)
        {
            StoreBytes(board_.tiles, offset, value, size);
            return;
        }
        if (const uint32_t offset = OffsetIn(paddr, kAleck64Palette, kAleck64VideoMemorySize);
            offset < kAleck64VideoMemorySize)
        {
            StoreBytes(board_.palette, offset, value, size);
        }
    }
}

} // namespace usf::n64
