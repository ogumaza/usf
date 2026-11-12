// SPDX-License-Identifier: MIT

// The FPU's arithmetic in each rounding mode against IEEE 754, which the VR4300's FPU follows for normal numbers. Each
// case runs one instruction on chosen operands and checks the result and FCR31: the rounding mode, the cause bits and
// the flags. The floating-point operands and results are normal numbers, apart from the zero divisor and the infinity
// of the division by zero.
//
// The expected values come from exact arithmetic on the operands and the standard's rounding rules, not from a run of
// the core. The four modes round to the nearest value with ties to even, toward zero, toward plus infinity and toward
// minus infinity. A result that isn't exact sets the cause bit and the flag for an inexact result.

#include <cstdint>
#include <string>

#include "xsflib/testing/check.h"

#include "n64/system.h"
#include "test_programs.h"

namespace
{

using usf::n64::System;
using usf::testing::kCode;
using usf::testing::kCodeAddress;

// Status with COP1 usable and 32 FPU registers of 64 bits.
constexpr uint32_t kStatusCop1 = usf::n64::kStatusCu1 | usf::n64::kStatusFr;

// FCR31's rounding modes, and its cause bit and flag for an inexact result and for a division by zero.
constexpr uint32_t kNearest = 0;
constexpr uint32_t kTowardZero = 1;
constexpr uint32_t kTowardPlus = 2;
constexpr uint32_t kTowardMinus = 3;
constexpr uint32_t kInexact = (1u << 12) | (1u << 2);
constexpr uint32_t kDivisionByZero = (1u << 15) | (1u << 5);

// COP1's formats and function codes.
constexpr uint32_t kSingle = 16;
constexpr uint32_t kDouble = 17;
constexpr uint32_t kWord = 20;
constexpr uint32_t kAdd = 0x00;
constexpr uint32_t kSub = 0x01;
constexpr uint32_t kMul = 0x02;
constexpr uint32_t kDiv = 0x03;
constexpr uint32_t kSqrt = 0x04;
constexpr uint32_t kCvtS = 0x20;
constexpr uint32_t kCvtW = 0x24;

// The masks of a result in $f0: a single-precision value or a word is its low 32 bits.
constexpr uint64_t kLow32 = 0xFFFFFFFF;
constexpr uint64_t kAll64 = ~uint64_t{0};

// $f0 = $f2 op $f4.
uint32_t Binary(uint32_t format, uint32_t funct)
{
    return 0x44000000 | (format << 21) | (4u << 16) | (2u << 11) | funct;
}

// $f0 = op $f2.
uint32_t Unary(uint32_t format, uint32_t funct)
{
    return 0x44000000 | (format << 21) | (2u << 11) | funct;
}

struct Case
{
    const char* name;
    uint32_t instruction;
    uint64_t first;
    uint64_t second;
    uint64_t mask;
    // The results in the four modes, in the order of FCR31's values.
    uint64_t results[4];
    uint32_t cause;
};

// Runs the case's instruction in `mode` with $f2 = first and $f4 = second, and checks $f0 and FCR31.
void CheckCase(const Case& test, uint32_t mode)
{
    System system;
    system.GetCpu().SetCop0(usf::n64::kCop0Status, kStatusCop1);
    system.GetCpu().SetFpr(2, test.first);
    system.GetCpu().SetFpr(4, test.second);
    system.GetCpu().SetFcr31(mode);
    usf::testing::WriteProgram(system, kCode, {test.instruction});
    system.GetCpu().SetPc(kCodeAddress);

    system.Run(system.TicksPerInstruction());

    const std::string what = std::string(test.name) + " in mode " + std::to_string(mode);
    xsflib::testing::Check((system.GetCpu().Fpr(0) & test.mask) == test.results[mode], what + ": the result");
    xsflib::testing::Check(system.GetCpu().Fcr31() == (mode | test.cause), what + ": FCR31");
}

void Arithmetic()
{
    const Case cases[] = {
        {"1 / 3 (div.s)",
         Binary(kSingle, kDiv),
         0x3F800000,
         0x40400000,
         kLow32,
         {0x3EAAAAAB, 0x3EAAAAAA, 0x3EAAAAAB, 0x3EAAAAAA},
         kInexact},
        {"-1 / 3 (div.s)",
         Binary(kSingle, kDiv),
         0xBF800000,
         0x40400000,
         kLow32,
         {0xBEAAAAAB, 0xBEAAAAAA, 0xBEAAAAAA, 0xBEAAAAAB},
         kInexact},
        {"(1 + 2^-23)^2 (mul.s)",
         Binary(kSingle, kMul),
         0x3F800001,
         0x3F800001,
         kLow32,
         {0x3F800002, 0x3F800002, 0x3F800003, 0x3F800002},
         kInexact},
        {"1 + 2^-53 (add.d)",
         Binary(kDouble, kAdd),
         0x3FF0000000000000,
         0x3CA0000000000000,
         kAll64,
         {0x3FF0000000000000, 0x3FF0000000000000, 0x3FF0000000000001, 0x3FF0000000000000},
         kInexact},
        {"-1 - 2^-53 (sub.d)",
         Binary(kDouble, kSub),
         0xBFF0000000000000,
         0x3CA0000000000000,
         kAll64,
         {0xBFF0000000000000, 0xBFF0000000000000, 0xBFF0000000000000, 0xBFF0000000000001},
         kInexact},
        {"the square root of 2 (sqrt.s)",
         Unary(kSingle, kSqrt),
         0x40000000,
         0,
         kLow32,
         {0x3FB504F3, 0x3FB504F3, 0x3FB504F4, 0x3FB504F3},
         kInexact},
        {"2.5 to a word (cvt.w.s)", Unary(kSingle, kCvtW), 0x40200000, 0, kLow32, {2, 2, 3, 2}, kInexact},
        {"-2.5 to a word (cvt.w.s)",
         Unary(kSingle, kCvtW),
         0xC0200000,
         0,
         kLow32,
         {0xFFFFFFFE, 0xFFFFFFFE, 0xFFFFFFFE, 0xFFFFFFFD},
         kInexact},
        {"2^24 + 1 to single precision (cvt.s.w)",
         Unary(kWord, kCvtS),
         0x01000001,
         0,
         kLow32,
         {0x4B800000, 0x4B800000, 0x4B800001, 0x4B800000},
         kInexact},
        {"1 + 2 (add.s)",
         Binary(kSingle, kAdd),
         0x3F800000,
         0x40000000,
         kLow32,
         {0x40400000, 0x40400000, 0x40400000, 0x40400000},
         0},
        {"1 / 0 (div.s)",
         Binary(kSingle, kDiv),
         0x3F800000,
         0x00000000,
         kLow32,
         {0x7F800000, 0x7F800000, 0x7F800000, 0x7F800000},
         kDivisionByZero},
    };

    for (const Case& test : cases)
    {
        for (const uint32_t mode : {kNearest, kTowardZero, kTowardPlus, kTowardMinus})
        {
            CheckCase(test, mode);
        }
    }
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("fpu", [] { Arithmetic(); });
}
