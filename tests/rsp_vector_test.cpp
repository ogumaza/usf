// SPDX-License-Identifier: MIT

// The RSP's vector multiplies and sums against the formulas on n64brew's pages about the RSP. A program runs each
// instruction on the same two vectors and stores its result and the three slices of the accumulator. The lanes reach
// the clamp of the result, the rounding of VMULF and the wrap of the 48-bit accumulator.
//
// The expected values come from the formulas alone, not from a run of the core. VMULF sets the accumulator to
// vs * vt * 2 + 0x8000, and VMACF adds vs * vt * 2. VMUDH sets it to vs * vt << 16, and VMADH adds vs * vt << 16. The
// accumulator keeps 48 bits. The result of each is bits 47 to 16 of the accumulator, clamped to 16 bits. VADD and VSUB
// clamp vs + vt and vs - vt, with no carry here, and put the sum's low 16 bits in the low slice. They leave the other
// slices alone. The values are hexadecimal, lane 0 first.

#include <array>
#include <cstdint>
#include <vector>

#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

#include "n64/rsp.h"
#include "n64/system.h"
#include "test_programs.h"

namespace
{

using Vector = std::array<uint16_t, 8>;

using usf::testing::kVadd;
using usf::testing::kVmacf;
using usf::testing::kVmadh;
using usf::testing::kVmudh;
using usf::testing::kVmulf;
using usf::testing::kVsar;
using usf::testing::kVsub;
using usf::testing::RspLqv;
using usf::testing::RspSqv;
using usf::testing::RspVector;

// The elements of VSAR that read the high, middle and low slices of the accumulator.
constexpr uint32_t kHighSlice = 8;
constexpr uint32_t kMiddleSlice = 9;
constexpr uint32_t kLowSlice = 10;

constexpr uint32_t kBreak = 0x0000000D;

// SP_STATUS by its index in the RSP's registers, and the bit of a write that clears the halt.
constexpr uint32_t kSpStatus = 4;
constexpr uint32_t kClearHalt = 1;

// Where the program keeps its two inputs and the results of each step in DMEM, in units of 16 bytes.
constexpr uint32_t kFirstInput = 0;
constexpr uint32_t kSecondInput = 1;
constexpr uint32_t kResults = 2;

constexpr Vector kVs = {0x8000, 0x7FFF, 0x4000, 0xC000, 0x0001, 0xFFFF, 0x1234, 0x8000};
constexpr Vector kVt = {0x8000, 0x7FFF, 0x4000, 0x4000, 0x0001, 0x0001, 0x5678, 0x7FFF};

// What one instruction leaves: its result, and the accumulator's slices.
struct Step
{
    Vector result{};
    Vector high{};
    Vector middle{};
    Vector low{};
};

void StoreVector(usf::n64::Rsp::SpMemory& dmem, uint32_t sixteens, const Vector& vector)
{
    for (size_t lane = 0; lane < vector.size(); lane++)
    {
        xsflib::StoreBe16(dmem.data() + 16 * sixteens + 2 * lane, vector[lane]);
    }
}

Vector LoadVector(const usf::n64::Rsp::SpMemory& dmem, uint32_t sixteens)
{
    Vector vector{};
    for (size_t lane = 0; lane < vector.size(); lane++)
    {
        vector[lane] = xsflib::LoadBe16(dmem.data() + 16 * sixteens + 2 * lane);
    }

    return vector;
}

// The program: $v1 and $v2 from DMEM, then each of `functs` into $v3, with VSAR's copies of the slices in $v4 to $v6.
// Each step stores the four vectors after the inputs.
std::vector<uint32_t> MakeProgram(const std::vector<uint32_t>& functs)
{
    std::vector<uint32_t> program = {RspLqv(1, kFirstInput), RspLqv(2, kSecondInput)};
    uint32_t out = kResults;
    for (const uint32_t funct : functs)
    {
        program.push_back(RspVector(funct, 3, 1, 2));
        program.push_back(RspVector(kVsar, 4, 0, 0, kHighSlice));
        program.push_back(RspVector(kVsar, 5, 0, 0, kMiddleSlice));
        program.push_back(RspVector(kVsar, 6, 0, 0, kLowSlice));
        for (uint32_t vector = 3; vector <= 6; vector++)
        {
            program.push_back(RspSqv(vector, out++));
        }
    }
    program.push_back(kBreak);

    return program;
}

// Runs `functs` in turn on kVs and kVt, from a reset RSP. Returns what each step left.
std::vector<Step> RunSteps(const std::vector<uint32_t>& functs)
{
    usf::n64::System system;
    usf::n64::Rsp& rsp = system.GetRsp();
    const std::vector<uint32_t> program = MakeProgram(functs);
    for (size_t i = 0; i < program.size(); i++)
    {
        xsflib::StoreBe32(rsp.Imem().data() + 4 * i, program[i]);
    }
    StoreVector(rsp.Dmem(), kFirstInput, kVs);
    StoreVector(rsp.Dmem(), kSecondInput, kVt);

    rsp.WritePc(0);
    rsp.WriteRegister(kSpStatus, kClearHalt);
    rsp.Run(program.size());

    XSFLIB_CHECK(rsp.Halted());
    std::vector<Step> steps(functs.size());
    uint32_t in = kResults;
    for (Step& step : steps)
    {
        step.result = LoadVector(rsp.Dmem(), in++);
        step.high = LoadVector(rsp.Dmem(), in++);
        step.middle = LoadVector(rsp.Dmem(), in++);
        step.low = LoadVector(rsp.Dmem(), in++);
    }

    return steps;
}

void CheckStep(const Step& step, const Step& expected)
{
    XSFLIB_CHECK(step.result == expected.result);
    XSFLIB_CHECK(step.high == expected.high);
    XSFLIB_CHECK(step.middle == expected.middle);
    XSFLIB_CHECK(step.low == expected.low);
}

// VMULF, then VMACF onto its accumulator. In lane 0, -1 times -1 gives 1. That is past the largest fraction, and the
// result clamps. In lane 7 of VMACF, the sum falls below -1 and clamps.
void MultiplyFractions()
{
    const std::vector<Step> steps = RunSteps({kVmulf, kVmacf});

    CheckStep(steps[0], {.result = {0x7FFF, 0x7FFE, 0x2000, 0xE000, 0x0000, 0x0000, 0x0C4C, 0x8001},
                         .high = {0x0000, 0x0000, 0x0000, 0xFFFF, 0x0000, 0x0000, 0x0000, 0xFFFF},
                         .middle = {0x8000, 0x7FFE, 0x2000, 0xE000, 0x0000, 0x0000, 0x0C4C, 0x8001},
                         .low = {0x8000, 0x8002, 0x8000, 0x8000, 0x8002, 0x7FFE, 0x80C0, 0x8000}});
    CheckStep(steps[1], {.result = {0x7FFF, 0x7FFF, 0x4000, 0xC000, 0x0000, 0x0000, 0x1898, 0x8000},
                         .high = {0x0001, 0x0000, 0x0000, 0xFFFF, 0x0000, 0x0000, 0x0000, 0xFFFF},
                         .middle = {0x0000, 0xFFFC, 0x4000, 0xC000, 0x0000, 0x0000, 0x1898, 0x0002},
                         .low = {0x8000, 0x8004, 0x8000, 0x8000, 0x8004, 0x7FFC, 0x8180, 0x8000}});
}

// VMUDH, then VMADH onto its accumulator. In lane 0 of VMADH, the sum reaches bit 47 and wraps to a negative value.
void MultiplyIntegers()
{
    const std::vector<Step> steps = RunSteps({kVmudh, kVmadh});

    CheckStep(steps[0], {.result = {0x7FFF, 0x7FFF, 0x7FFF, 0x8000, 0x0001, 0xFFFF, 0x7FFF, 0x8000},
                         .high = {0x4000, 0x3FFF, 0x1000, 0xF000, 0x0000, 0xFFFF, 0x0626, 0xC000},
                         .middle = {0x0000, 0x0001, 0x0000, 0x0000, 0x0001, 0xFFFF, 0x0060, 0x8000},
                         .low = {0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000}});
    CheckStep(steps[1], {.result = {0x8000, 0x7FFF, 0x7FFF, 0x8000, 0x0002, 0xFFFE, 0x7FFF, 0x8000},
                         .high = {0x8000, 0x7FFE, 0x2000, 0xE000, 0x0000, 0xFFFF, 0x0C4C, 0x8001},
                         .middle = {0x0000, 0x0002, 0x0000, 0x0000, 0x0002, 0xFFFE, 0x00C0, 0x0000},
                         .low = {0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000}});
}

// VADD and VSUB after VMUDH and VMADH: the low slice takes the sum, and the other two keep VMADH's values.
void AddAndSubtract()
{
    const std::vector<Step> steps = RunSteps({kVmudh, kVmadh, kVadd, kVsub});

    CheckStep(steps[2], {.result = {0x8000, 0x7FFF, 0x7FFF, 0x0000, 0x0002, 0x0000, 0x68AC, 0xFFFF},
                         .high = {0x8000, 0x7FFE, 0x2000, 0xE000, 0x0000, 0xFFFF, 0x0C4C, 0x8001},
                         .middle = {0x0000, 0x0002, 0x0000, 0x0000, 0x0002, 0xFFFE, 0x00C0, 0x0000},
                         .low = {0x0000, 0xFFFE, 0x8000, 0x0000, 0x0002, 0x0000, 0x68AC, 0xFFFF}});
    CheckStep(steps[3], {.result = {0x0000, 0x0000, 0x0000, 0x8000, 0x0000, 0xFFFE, 0xBBBC, 0x8000},
                         .high = {0x8000, 0x7FFE, 0x2000, 0xE000, 0x0000, 0xFFFF, 0x0C4C, 0x8001},
                         .middle = {0x0000, 0x0002, 0x0000, 0x0000, 0x0002, 0xFFFE, 0x00C0, 0x0000},
                         .low = {0x0000, 0x0000, 0x0000, 0x8000, 0x0000, 0xFFFE, 0xBBBC, 0x0001}});
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("rsp_vector",
                                    []
                                    {
                                        MultiplyFractions();
                                        MultiplyIntegers();
                                        AddAndSubtract();
                                    });
}
