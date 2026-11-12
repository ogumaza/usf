// SPDX-License-Identifier: MIT

#include "n64/coverage.h"

#include <algorithm>

namespace usf::n64
{

Coverage::Coverage(uint32_t rdram_size)
    : rdram_size_(rdram_size), rdram_written_(rdram_size / 4), rdram_needed_(rdram_size)
{
}

void Coverage::ReadRom(uint32_t offset, uint32_t length)
{
    if (length == 0)
    {
        return;
    }

    const uint32_t first = offset / 4;
    const uint32_t last = (offset + length - 1) / 4;
    rom_read_.Grow(last + 1);
    for (uint32_t word = first; word <= last; word++)
    {
        rom_read_.AddWord(word);
    }
}

void Coverage::ReadRdram(uint32_t address, uint32_t length)
{
    const uint32_t end = std::min(address + length, rdram_size_);
    while (address < end)
    {
        // The bytes of this word that the access reads.
        const uint32_t word = address / 4;
        const uint32_t word_end = std::min(end, (word + 1) * 4);
        uint32_t mask = 0;
        for (uint32_t byte = address; byte < word_end; byte++)
        {
            mask |= 1u << (byte % 4);
        }

        if ((mask & ~rdram_written_[word]) != 0)
        {
            rdram_needed_.AddWord(word);
        }

        address = word_end;
    }
}

void Coverage::WriteRdram(uint32_t address, uint32_t length)
{
    const uint32_t end = std::min(address + length, rdram_size_);
    for (uint32_t byte = address; byte < end; byte++)
    {
        rdram_written_[byte / 4] |= static_cast<uint8_t>(1u << (byte % 4));
    }
}

const WordSet& Coverage::RomWords() const
{
    return rom_read_;
}

const WordSet& Coverage::RdramWords() const
{
    return rdram_needed_;
}

} // namespace usf::n64
