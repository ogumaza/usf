// SPDX-License-Identifier: MIT

#include "n64/coverage.h"

#include <algorithm>
#include <bit>

namespace usf::n64
{

Coverage::Coverage(uint32_t rdram_size)
    : rdram_size_(rdram_size), rdram_written_(rdram_size / 4), rdram_needed_(rdram_size / 32)
{
}

void Coverage::Clear()
{
    std::fill(rom_read_.begin(), rom_read_.end(), uint8_t{0});
    std::fill(rdram_written_.begin(), rdram_written_.end(), uint8_t{0});
    std::fill(rdram_needed_.begin(), rdram_needed_.end(), uint8_t{0});
}

void Coverage::ReadRom(uint32_t offset, uint32_t length)
{
    if (length == 0)
    {
        return;
    }

    const uint32_t first = offset / 4;
    const uint32_t last = (offset + length - 1) / 4;
    if (last / 8 >= rom_read_.size())
    {
        rom_read_.resize(last / 8 + 1);
    }

    for (uint32_t word = first; word <= last; word++)
    {
        rom_read_[word / 8] |= static_cast<uint8_t>(1u << (word % 8));
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
            rdram_needed_[word / 8] |= static_cast<uint8_t>(1u << (word % 8));
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

std::vector<ByteRange> Coverage::Ranges(const std::vector<uint8_t>& bits)
{
    std::vector<ByteRange> ranges;
    const uint32_t words = static_cast<uint32_t>(bits.size() * 8);
    uint32_t word = 0;
    while (word < words)
    {
        if ((bits[word / 8] & (1u << (word % 8))) == 0)
        {
            word++;
            continue;
        }

        const uint32_t start = word;
        while (word < words && (bits[word / 8] & (1u << (word % 8))) != 0)
        {
            word++;
        }
        ranges.push_back({start * 4, (word - start) * 4});
    }

    return ranges;
}

std::vector<ByteRange> Coverage::RomRanges() const
{
    return Ranges(rom_read_);
}

std::vector<ByteRange> Coverage::RdramRanges() const
{
    return Ranges(rdram_needed_);
}

bool Coverage::RomWordRead(uint32_t offset) const
{
    const uint32_t word = offset / 4;
    return word / 8 < rom_read_.size() && (rom_read_[word / 8] & (1u << (word % 8))) != 0;
}

bool Coverage::RdramWordNeeded(uint32_t address) const
{
    const uint32_t word = address / 4;
    return word / 8 < rdram_needed_.size() && (rdram_needed_[word / 8] & (1u << (word % 8))) != 0;
}

uint64_t Coverage::RomWordsRead() const
{
    uint64_t count = 0;
    for (uint8_t byte : rom_read_)
    {
        count += static_cast<uint64_t>(std::popcount(byte));
    }

    return count;
}

uint64_t Coverage::RdramWordsNeeded() const
{
    uint64_t count = 0;
    for (uint8_t byte : rdram_needed_)
    {
        count += static_cast<uint64_t>(std::popcount(byte));
    }

    return count;
}

} // namespace usf::n64
