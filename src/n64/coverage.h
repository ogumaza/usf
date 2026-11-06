// SPDX-License-Identifier: MIT

// Coverage for trimming a rip: which ROM words the CPU or the PI read, and which RDRAM words are read before they're
// written. A word of RDRAM that's read before anything writes it comes from the save state, so the rip must keep it. A
// word that's written first needs nothing from the state.

#pragma once

#include <cstdint>
#include <vector>

namespace usf::n64
{

// A range of bytes [offset, offset + length).
struct ByteRange
{
    uint32_t offset;
    uint32_t length;
};

class Coverage
{
public:
    explicit Coverage(uint32_t rdram_size);

    // Forgets everything recorded so far.
    void Clear();

    // Records accesses. Offsets are cartridge offsets for the ROM and physical addresses for RDRAM.
    void ReadRom(uint32_t offset, uint32_t length);
    void ReadRdram(uint32_t address, uint32_t length);
    void WriteRdram(uint32_t address, uint32_t length);

    // The results, as ranges of whole 4-byte words in ascending order.
    std::vector<ByteRange> RomRanges() const;
    std::vector<ByteRange> RdramRanges() const;
    bool RomWordRead(uint32_t offset) const;
    bool RdramWordNeeded(uint32_t address) const;
    uint64_t RomWordsRead() const;
    uint64_t RdramWordsNeeded() const;

private:
    static std::vector<ByteRange> Ranges(const std::vector<uint8_t>& bits);

    uint32_t rdram_size_;
    std::vector<uint8_t> rom_read_;      // One bit for each ROM word.
    std::vector<uint8_t> rdram_written_; // Four bits for each RDRAM word, one for each byte written.
    std::vector<uint8_t> rdram_needed_;  // One bit for each RDRAM word.
};

} // namespace usf::n64
