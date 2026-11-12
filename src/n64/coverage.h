// SPDX-License-Identifier: MIT

// Coverage for trimming a rip: which ROM words the CPU or the PI read, and which RDRAM words are read before they're
// written. A word of RDRAM that's read before anything writes it comes from the save state, so the rip must keep it. A
// word that's written first needs nothing from the state.

#pragma once

#include <cstdint>
#include <vector>

#include "n64/word_set.h"

namespace usf::n64
{

class Coverage
{
public:
    explicit Coverage(uint32_t rdram_size);

    // Records accesses. Offsets are cartridge offsets for the ROM and physical addresses for RDRAM.
    void ReadRom(uint32_t offset, uint32_t length);
    void ReadRdram(uint32_t address, uint32_t length);
    void WriteRdram(uint32_t address, uint32_t length);

    // The results: the ROM words read, and the RDRAM words read before anything wrote them.
    const WordSet& RomWords() const;
    const WordSet& RdramWords() const;

private:
    uint32_t rdram_size_;
    WordSet rom_read_;                   // It grows to the last ROM word read.
    std::vector<uint8_t> rdram_written_; // Four bits for each RDRAM word, one for each byte written.
    WordSet rdram_needed_;
};

} // namespace usf::n64
