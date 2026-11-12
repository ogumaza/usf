// SPDX-License-Identifier: MIT

// The MIPS general-purpose registers by their O32 names, and the fields of a jump.

#pragma once

#include <cstdint>

namespace usf::n64::mips
{

enum Register : uint32_t
{
    kZero = 0,
    kAt = 1,
    kV0 = 2,
    kV1 = 3,
    kA0 = 4,
    kA1 = 5,
    kA2 = 6,
    kA3 = 7,
    kT0 = 8,
    kT1 = 9,
    kT2 = 10,
    kT3 = 11,
    kT4 = 12,
    kT5 = 13,
    kT6 = 14,
    kT7 = 15,
    kS0 = 16,
    kS1 = 17,
    kS2 = 18,
    kS3 = 19,
    kS4 = 20,
    kS5 = 21,
    kS6 = 22,
    kS7 = 23,
    kT8 = 24,
    kT9 = 25,
    kSp = 29,
    kRa = 31,
};

// J and JAL: the bits of the delay slot's address that the target keeps, and the instruction's field of the target's
// other bits, in words.
inline constexpr uint64_t kJumpRegionMask = 0xFFFFFFFFF0000000ull;
inline constexpr uint32_t kJumpIndexMask = 0x03FFFFFF;

} // namespace usf::n64::mips
