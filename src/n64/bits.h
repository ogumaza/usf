// SPDX-License-Identifier: MIT

// Small helpers for the bits and bytes of the core's registers and memory.

#pragma once

#include <cstdint>

namespace usf::n64
{

// The low 32 bits of `value`, sign-extended to 64 bits. The VR4300's 32-bit operations write their results in this
// form.
constexpr uint64_t SignExtend32(uint64_t value)
{
    return static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(static_cast<uint32_t>(value))));
}

// Stores the low `size` bytes of `value` at `p`, in big-endian order.
inline void StoreBe(uint8_t* p, uint64_t value, int size)
{
    for (int i = size - 1; i >= 0; i--)
    {
        p[i] = static_cast<uint8_t>(value);
        value >>= 8;
    }
}

} // namespace usf::n64
