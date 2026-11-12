// SPDX-License-Identifier: CC0-1.0

// The RSP's vector unit: 32 vector registers of eight 16-bit lanes, the 48-bit accumulator of each lane, the flags
// registers VCO, VCC and VCE, and the divider with its reciprocal tables. This file is adapted from Iconoclast's RSP
// (mupen64plus-rsp-cxd4, its vu folder), which is dedicated to the public domain under CC0 1.0. These changes are
// dedicated the same way.

#pragma once

#include <array>
#include <cstdint>

namespace usf::n64
{

class VectorUnit
{
public:
    // What a computational instruction does, from its fields. `element` is the 4-bit element field.
    using Operation = void (*)(VectorUnit& unit, int vd, int vs, int vt, int element);

    // The operation of a computational instruction: COP2 with bit 25 set.
    static Operation Decode(uint32_t instruction);

    void Reset();

    // A register's bytes in console order: byte 0 is the high byte of lane 0.
    uint8_t Byte(int reg, int index) const
    {
        const uint16_t lane = vr_[reg][index / 2];
        return static_cast<uint8_t>((index & 1) != 0 ? lane : lane >> 8);
    }

    void SetByte(int reg, int index, uint8_t value)
    {
        uint16_t& lane = vr_[reg][index / 2];
        if ((index & 1) != 0)
        {
            lane = static_cast<uint16_t>((lane & 0xFF00) | value);
        }
        else
        {
            lane = static_cast<uint16_t>((lane & 0x00FF) | (value << 8));
        }
    }

    uint16_t Lane(int reg, int lane) const
    {
        return vr_[reg][lane];
    }

    void SetLane(int reg, int lane, uint16_t value)
    {
        vr_[reg][lane] = value;
    }

    // CFC2 and CTC2. CFC2 sign-extends the 16-bit value.
    uint32_t ReadControl(int index) const;
    void WriteControl(int index, uint32_t value);

private:
    using Vector = std::array<uint16_t, 8>;

    Vector Broadcast(int reg, int element) const;
    int64_t Acc(int lane) const;
    void SetAcc(int lane, int64_t value);
    uint16_t AccHigh(int lane) const;
    uint16_t AccMid(int lane) const;
    uint16_t AccLow(int lane) const;
    void SetAccLow(int lane, uint16_t value);

    // The clamps that move the accumulator into a register.
    uint16_t SaturateMid(int lane) const;
    uint16_t SaturateLow(int lane) const;

    // The operations that take vt with the element's broadcast. Each has its opcode as a template argument, so that the
    // choice of operation stays out of the loop over the lanes.
    template <uint32_t kOp>
    void Multiply(int vd, int vs, const Vector& vt);
    template <uint32_t kOp>
    void AddSubtract(int vd, int vs, const Vector& vt);
    template <uint32_t kOp>
    void Select(int vd, int vs, const Vector& vt);
    template <uint32_t kOp>
    void Logical(int vd, int vs, const Vector& vt);
    template <bool kPositive>
    void Round(int vd, int vs_field, const Vector& vt);
    void Reserved(int vd, int vs, const Vector& vt);
    template <void (VectorUnit::*kMethod)(int, int, const Vector&)>
    static void WithBroadcast(VectorUnit& unit, int vd, int vs, int vt, int element);

    // The other operations.
    template <uint32_t kOp>
    static void Divide(VectorUnit& unit, int vd, int vs, int vt, int element);
    static void MultiplyAccumulateQ(VectorUnit& unit, int vd, int vs, int vt, int element);
    static void AccumulatorSlice(VectorUnit& unit, int vd, int vs, int vt, int element);
    static void NoOperation(VectorUnit& unit, int vd, int vs, int vt, int element);

    int32_t Reciprocal(int32_t input, bool square_root) const;

    std::array<Vector, 32> vr_{};
    std::array<int64_t, 8> acc_{};
    uint16_t vco_ = 0; // Bits 0 to 7: carry. Bits 8 to 15: not equal.
    uint16_t vcc_ = 0; // Bits 0 to 7: compare. Bits 8 to 15: clip.
    uint8_t vce_ = 0;
    int32_t div_in_ = 0;
    int32_t div_out_ = 0;
    bool div_double_ = false; // The last divide was VRCPH or VRSQH, so the next VRCPL or VRSQL takes 32 bits.
};

} // namespace usf::n64
