// SPDX-License-Identifier: MIT

// The parts of a Project64 1.4 save state that the USF files and the ripper read too.

#pragma once

#include <cstddef>
#include <cstdint>

#include "n64/constants.h"

namespace usf::n64
{

// The first word of a state.
inline constexpr uint32_t kStateId = 0x23D8A6C8;

// Where a state keeps the size of its RDRAM, and the CPU's general-purpose registers, 8 bytes each.
inline constexpr uint32_t kStateRdramSizeOffset = 0x004;
inline constexpr uint32_t kStateGprOffset = 0x050;

// Where a state keeps the TLB's 32 entries, 20 bytes each.
inline constexpr uint32_t kStateTlbOffset = 0x49C;

// Where RDRAM starts in a state. The header comes before it, and DMEM and IMEM come after it.
inline constexpr uint32_t kStateRdramOffset = 0x75C;

// How much larger a state is than the RDRAM in it.
inline constexpr uint32_t kStateOverhead = kStateRdramOffset + 2 * kSpMemSize;

// Copies memory in console order to or from LE word order.
inline void CopyToLeWords(const uint8_t* source, uint8_t* target, size_t size)
{
    for (size_t i = 0; i < size; i++)
    {
        target[i ^ 3] = source[i];
    }
}

inline void CopyFromLeWords(const uint8_t* source, uint8_t* target, size_t size)
{
    for (size_t i = 0; i < size; i++)
    {
        target[i] = source[i ^ 3];
    }
}

} // namespace usf::n64
