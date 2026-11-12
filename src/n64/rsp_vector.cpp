// SPDX-License-Identifier: CC0-1.0

// The RSP's vector unit. Adapted from Iconoclast's RSP (mupen64plus-rsp-cxd4, the files vu/vu.c, vu/add.c,
// vu/multiply.c, vu/select.c, vu/logical.c and vu/divide.c), which are dedicated to the public domain under CC0 1.0.
// The divider's use of its table, the clamps and the clip compares follow that code. The table itself is computed from
// the reciprocals and inverse square roots that it holds. The lanes and the accumulator are rewritten as plain arrays.
// The opcodes that Iconoclast's RSP treats as reserved (VMULQ, VMACQ, VRNDP and VRNDN) follow the behaviour that the
// hardware tests record. These changes are dedicated to the public domain under CC0 1.0 too.

#include "n64/rsp_vector.h"

#include <algorithm>
#include <bit>

namespace usf::n64
{
namespace
{

// The largest integer whose square is at most `value`, digit by digit in base 4.
constexpr uint64_t IntegerSqrt(uint64_t value)
{
    uint64_t root = 0;
    for (uint64_t bit = uint64_t{1} << 62; bit != 0; bit >>= 2)
    {
        if (value >= root + bit)
        {
            value -= root + bit;
            root = (root >> 1) + bit;
        }
        else
        {
            root >>= 1;
        }
    }

    return root;
}

// The divider's table: 16-bit fractions below an implied leading one, clamped to 0xFFFF. The first 512 entries are
// reciprocals, for the nine bits i after the leading one of the input: (2^34 / (512 + i) + 1) / 256, with each division
// rounded down. The last 512 entries are inverse square roots. Entry 2k + p is for the eight bits k after the leading
// one and the parity p of the input's count of leading zeros: the square root of 2^(41 + p) / (256 + k), rounded down.
constexpr std::array<uint16_t, 1024> MakeDivideTable()
{
    std::array<uint16_t, 1024> table = {};
    for (uint64_t i = 0; i < 512; i++)
    {
        const uint64_t reciprocal = (((uint64_t{1} << 34) / (512 + i)) + 1) >> 8;
        table[i] = static_cast<uint16_t>(std::min<uint64_t>(reciprocal - 0x10000, 0xFFFF));
    }

    for (uint64_t j = 0; j < 512; j++)
    {
        const uint64_t root = IntegerSqrt((uint64_t{1} << (41 + (j & 1))) / (256 + j / 2));
        table[512 + j] = static_cast<uint16_t>(std::min<uint64_t>(root - 0x10000, 0xFFFF));
    }

    return table;
}

constexpr std::array<uint16_t, 1024> kDivideTable = MakeDivideTable();

// The lanes that each element field selects from vt, for the computational instructions.
constexpr std::array<std::array<uint8_t, 8>, 16> kBroadcast = {{
    {0, 1, 2, 3, 4, 5, 6, 7},
    {0, 1, 2, 3, 4, 5, 6, 7},
    {0, 0, 2, 2, 4, 4, 6, 6},
    {1, 1, 3, 3, 5, 5, 7, 7},
    {0, 0, 0, 0, 4, 4, 4, 4},
    {1, 1, 1, 1, 5, 5, 5, 5},
    {2, 2, 2, 2, 6, 6, 6, 6},
    {3, 3, 3, 3, 7, 7, 7, 7},
    {0, 0, 0, 0, 0, 0, 0, 0},
    {1, 1, 1, 1, 1, 1, 1, 1},
    {2, 2, 2, 2, 2, 2, 2, 2},
    {3, 3, 3, 3, 3, 3, 3, 3},
    {4, 4, 4, 4, 4, 4, 4, 4},
    {5, 5, 5, 5, 5, 5, 5, 5},
    {6, 6, 6, 6, 6, 6, 6, 6},
    {7, 7, 7, 7, 7, 7, 7, 7},
}};

int16_t Signed(uint16_t value)
{
    return static_cast<int16_t>(value);
}

uint16_t ClampSigned(int32_t value)
{
    return static_cast<uint16_t>(std::clamp(value, -32768, 32767));
}

bool Bit(uint32_t value, int bit)
{
    return ((value >> bit) & 1) != 0;
}

uint32_t SetBit(uint32_t value, int bit, bool set)
{
    return set ? value | (1u << bit) : value & ~(1u << bit);
}

} // namespace

void VectorUnit::Reset()
{
    vr_ = {};
    acc_ = {};
    vco_ = 0;
    vcc_ = 0;
    vce_ = 0;
    div_in_ = 0;
    div_out_ = 0;
    div_double_ = false;
}

uint32_t VectorUnit::ReadControl(int index) const
{
    uint32_t value = 0;
    switch (index & 3)
    {
    case 0:
        value = vco_;
        break;
    case 1:
        value = vcc_;
        break;
    default:
        value = vce_;
        break;
    }

    return static_cast<uint32_t>(static_cast<int32_t>(Signed(static_cast<uint16_t>(value))));
}

void VectorUnit::WriteControl(int index, uint32_t value)
{
    switch (index & 3)
    {
    case 0:
        vco_ = static_cast<uint16_t>(value);
        break;
    case 1:
        vcc_ = static_cast<uint16_t>(value);
        break;
    default:
        vce_ = static_cast<uint8_t>(value);
        break;
    }
}

VectorUnit::Vector VectorUnit::Broadcast(int reg, int element) const
{
    if (element < 2)
    {
        return vr_[reg];
    }

    Vector result;
    for (int n = 0; n < 8; n++)
    {
        result[n] = vr_[reg][kBroadcast[element][n]];
    }

    return result;
}

int64_t VectorUnit::Acc(int lane) const
{
    return acc_[lane];
}

void VectorUnit::SetAcc(int lane, int64_t value)
{
    // Keep 48 bits, sign-extended.
    const uint64_t bits = static_cast<uint64_t>(value) & 0xFFFFFFFFFFFFull;
    acc_[lane] = static_cast<int64_t>(bits << 16) >> 16;
}

uint16_t VectorUnit::AccHigh(int lane) const
{
    return static_cast<uint16_t>(static_cast<uint64_t>(acc_[lane]) >> 32);
}

uint16_t VectorUnit::AccMid(int lane) const
{
    return static_cast<uint16_t>(static_cast<uint64_t>(acc_[lane]) >> 16);
}

uint16_t VectorUnit::AccLow(int lane) const
{
    return static_cast<uint16_t>(acc_[lane]);
}

void VectorUnit::SetAccLow(int lane, uint16_t value)
{
    acc_[lane] = (acc_[lane] & ~int64_t{0xFFFF}) | value;
}

uint16_t VectorUnit::SaturateMid(int lane) const
{
    // Bits 47 to 16 of the accumulator, clamped to 16 bits.
    return static_cast<uint16_t>(std::clamp<int64_t>(acc_[lane] >> 16, -32768, 32767));
}

uint16_t VectorUnit::SaturateLow(int lane) const
{
    // The low 16 bits when bits 47 to 31 hold a sign extension, otherwise 0 or 0xFFFF.
    const int64_t value = acc_[lane] >> 16;
    const uint16_t low = value > 32767 ? 0xFFFF : AccLow(lane);

    return value < -32768 ? 0x0000 : low;
}

VectorUnit::Operation VectorUnit::Decode(uint32_t instruction)
{
    switch (instruction & 0x3F)
    {
    case 0x00:
        return WithBroadcast<&VectorUnit::Multiply<0x00>>;
    case 0x01:
        return WithBroadcast<&VectorUnit::Multiply<0x01>>;
    case 0x02:
        return WithBroadcast<&VectorUnit::Round<true>>;
    case 0x03:
        return WithBroadcast<&VectorUnit::Multiply<0x03>>;
    case 0x04:
        return WithBroadcast<&VectorUnit::Multiply<0x04>>;
    case 0x05:
        return WithBroadcast<&VectorUnit::Multiply<0x05>>;
    case 0x06:
        return WithBroadcast<&VectorUnit::Multiply<0x06>>;
    case 0x07:
        return WithBroadcast<&VectorUnit::Multiply<0x07>>;
    case 0x08:
        return WithBroadcast<&VectorUnit::Multiply<0x08>>;
    case 0x09:
        return WithBroadcast<&VectorUnit::Multiply<0x09>>;
    case 0x0A:
        return WithBroadcast<&VectorUnit::Round<false>>;
    case 0x0B:
        return MultiplyAccumulateQ;
    case 0x0C:
        return WithBroadcast<&VectorUnit::Multiply<0x0C>>;
    case 0x0D:
        return WithBroadcast<&VectorUnit::Multiply<0x0D>>;
    case 0x0E:
        return WithBroadcast<&VectorUnit::Multiply<0x0E>>;
    case 0x0F:
        return WithBroadcast<&VectorUnit::Multiply<0x0F>>;
    case 0x10:
        return WithBroadcast<&VectorUnit::AddSubtract<0x10>>;
    case 0x11:
        return WithBroadcast<&VectorUnit::AddSubtract<0x11>>;
    case 0x13:
        return WithBroadcast<&VectorUnit::AddSubtract<0x13>>;
    case 0x14:
        return WithBroadcast<&VectorUnit::AddSubtract<0x14>>;
    case 0x15:
        return WithBroadcast<&VectorUnit::AddSubtract<0x15>>;
    case 0x1D:
        return AccumulatorSlice;
    case 0x20:
        return WithBroadcast<&VectorUnit::Select<0x20>>;
    case 0x21:
        return WithBroadcast<&VectorUnit::Select<0x21>>;
    case 0x22:
        return WithBroadcast<&VectorUnit::Select<0x22>>;
    case 0x23:
        return WithBroadcast<&VectorUnit::Select<0x23>>;
    case 0x24:
        return WithBroadcast<&VectorUnit::Select<0x24>>;
    case 0x25:
        return WithBroadcast<&VectorUnit::Select<0x25>>;
    case 0x26:
        return WithBroadcast<&VectorUnit::Select<0x26>>;
    case 0x27:
        return WithBroadcast<&VectorUnit::Select<0x27>>;
    case 0x28:
        return WithBroadcast<&VectorUnit::Logical<0x28>>;
    case 0x29:
        return WithBroadcast<&VectorUnit::Logical<0x29>>;
    case 0x2A:
        return WithBroadcast<&VectorUnit::Logical<0x2A>>;
    case 0x2B:
        return WithBroadcast<&VectorUnit::Logical<0x2B>>;
    case 0x2C:
        return WithBroadcast<&VectorUnit::Logical<0x2C>>;
    case 0x2D:
        return WithBroadcast<&VectorUnit::Logical<0x2D>>;
    case 0x30:
        return Divide<0x30>;
    case 0x31:
        return Divide<0x31>;
    case 0x32:
        return Divide<0x32>;
    case 0x33:
        return Divide<0x33>;
    case 0x34:
        return Divide<0x34>;
    case 0x35:
        return Divide<0x35>;
    case 0x36:
        return Divide<0x36>;
    case 0x37:
    case 0x3F:
        return NoOperation; // VNOP and VNULL.
    default:
        return WithBroadcast<&VectorUnit::Reserved>;
    }
}

template <void (VectorUnit::*kMethod)(int, int, const VectorUnit::Vector&)>
void VectorUnit::WithBroadcast(VectorUnit& unit, int vd, int vs, int vt, int element)
{
    const Vector vte = unit.Broadcast(vt, element);
    (unit.*kMethod)(vd, vs, vte);
}

void VectorUnit::NoOperation(VectorUnit&, int, int, int, int)
{
}

void VectorUnit::AccumulatorSlice(VectorUnit& unit, int vd, int, int, int element)
{
    // VSAR: one slice of the accumulator. Other elements read zero.
    for (int n = 0; n < 8; n++)
    {
        uint16_t value = 0;
        if (element == 8)
        {
            value = unit.AccHigh(n);
        }
        else if (element == 9)
        {
            value = unit.AccMid(n);
        }
        else if (element == 10)
        {
            value = unit.AccLow(n);
        }

        unit.vr_[vd][n] = value;
    }
}

template <uint32_t kOp>
void VectorUnit::Multiply(int vd, int vs, const Vector& vt)
{
    Vector result;
    for (int n = 0; n < 8; n++)
    {
        const int64_t s = Signed(vr_[vs][n]);
        const int64_t t = Signed(vt[n]);
        const int64_t us = vr_[vs][n];
        const int64_t ut = vt[n];
        if constexpr (kOp == 0x00) // VMULF
        {
            SetAcc(n, s * t * 2 + 0x8000);
            result[n] = SaturateMid(n);
        }
        else if constexpr (kOp == 0x01) // VMULU
        {
            SetAcc(n, s * t * 2 + 0x8000);
            result[n] = Signed(AccHigh(n)) < 0 ? 0x0000 : ((AccHigh(n) ^ AccMid(n)) & 0x8000) != 0 ? 0xFFFF : AccMid(n);
        }
        else if constexpr (kOp == 0x03)
        {
            // VMULQ: rounds a negative product toward zero by 31/32 and keeps it in bits 47 to 16.
            int32_t product = static_cast<int32_t>(s * t);
            if (product < 0)
            {
                product += 31;
            }

            SetAcc(n, static_cast<int64_t>(product) * 65536);
            result[n] = static_cast<uint16_t>(ClampSigned(product >> 1) & ~15u);
        }
        else if constexpr (kOp == 0x04) // VMUDL
        {
            SetAcc(n, (us * ut) >> 16);
            result[n] = AccLow(n);
        }
        else if constexpr (kOp == 0x05) // VMUDM
        {
            SetAcc(n, s * ut);
            result[n] = AccMid(n);
        }
        else if constexpr (kOp == 0x06) // VMUDN
        {
            SetAcc(n, us * t);
            result[n] = AccLow(n);
        }
        else if constexpr (kOp == 0x07) // VMUDH
        {
            SetAcc(n, (s * t) * 65536);
            result[n] = SaturateMid(n);
        }
        else if constexpr (kOp == 0x08) // VMACF
        {
            SetAcc(n, Acc(n) + s * t * 2);
            result[n] = SaturateMid(n);
        }
        else if constexpr (kOp == 0x09) // VMACU
        {
            SetAcc(n, Acc(n) + s * t * 2);
            result[n] = Signed(AccHigh(n)) < 0                       ? 0x0000
                        : (AccHigh(n) != 0 || Signed(AccMid(n)) < 0) ? 0xFFFF
                                                                     : AccMid(n);
        }
        else if constexpr (kOp == 0x0C) // VMADL
        {
            SetAcc(n, Acc(n) + ((us * ut) >> 16));
            result[n] = SaturateLow(n);
        }
        else if constexpr (kOp == 0x0D) // VMADM
        {
            SetAcc(n, Acc(n) + s * ut);
            result[n] = SaturateMid(n);
        }
        else if constexpr (kOp == 0x0E) // VMADN
        {
            SetAcc(n, Acc(n) + us * t);
            result[n] = SaturateLow(n);
        }
        else // VMADH
        {
            SetAcc(n, Acc(n) + (s * t) * 65536);
            result[n] = SaturateMid(n);
        }
    }

    vr_[vd] = result;
}

template <bool kPositive>
void VectorUnit::Round(int vd, int vs_field, const Vector& vt)
{
    // VRNDP adds vt to an accumulator that's zero or positive, VRNDN to one that's negative. An odd vs field shifts vt
    // into the middle slice.
    Vector result;
    for (int n = 0; n < 8; n++)
    {
        int64_t addend = Signed(vt[n]);
        if ((vs_field & 1) != 0)
        {
            addend *= 65536;
        }

        const bool applies = kPositive ? Acc(n) >= 0 : Acc(n) < 0;
        if (applies)
        {
            SetAcc(n, Acc(n) + addend);
        }

        result[n] = SaturateMid(n);
    }

    vr_[vd] = result;
}

void VectorUnit::MultiplyAccumulateQ(VectorUnit& unit, int vd, int, int, int)
{
    // VMACQ: moves bits 47 to 16 of the accumulator by 32 toward zero, unless bit 5 is already set, and stores them
    // shifted right by one with the low four bits clear.
    Vector result;
    for (int n = 0; n < 8; n++)
    {
        int32_t product = static_cast<int32_t>(static_cast<uint32_t>(static_cast<uint64_t>(unit.acc_[n]) >> 16));
        if (product < 0 && (product & 32) == 0)
        {
            product += 32;
        }
        else if (product >= 32 && (product & 32) == 0)
        {
            product -= 32;
        }

        const int64_t low = unit.AccLow(n);
        unit.SetAcc(n, static_cast<int64_t>(product) * 65536 + low);
        result[n] = static_cast<uint16_t>(ClampSigned(product >> 1) & ~15u);
    }

    unit.vr_[vd] = result;
}

template <uint32_t kOp>
void VectorUnit::AddSubtract(int vd, int vs, const Vector& vt)
{
    Vector result;
    uint16_t vco = 0;
    for (int n = 0; n < 8; n++)
    {
        const int32_t s = Signed(vr_[vs][n]);
        const int32_t t = Signed(vt[n]);
        const uint32_t us = vr_[vs][n];
        const uint32_t ut = vt[n];
        const int32_t carry = Bit(vco_, n) ? 1 : 0;
        if constexpr (kOp == 0x10)
        {
            // VADD: the sum with the carry in, clamped. The accumulator keeps the low 16 bits.
            const int32_t sum = s + t + carry;
            SetAccLow(n, static_cast<uint16_t>(sum));
            result[n] = ClampSigned(sum);
        }
        else if constexpr (kOp == 0x11)
        {
            // VSUB.
            const int32_t difference = s - t - carry;
            SetAccLow(n, static_cast<uint16_t>(difference));
            result[n] = ClampSigned(difference);
        }
        else if constexpr (kOp == 0x13)
        {
            // VABS: vt with the sign of vs. -32768 becomes 32767 in the register and stays in the accumulator.
            int32_t value = 0;
            if (s < 0)
            {
                value = -t;
            }
            else if (s > 0)
            {
                value = t;
            }

            SetAccLow(n, static_cast<uint16_t>(value));
            result[n] = value == 32768 ? 0x7FFF : static_cast<uint16_t>(value);
        }
        else if constexpr (kOp == 0x14)
        {
            // VADDC: the unsigned sum, with its carry in VCO.
            const uint32_t sum = us + ut;
            SetAccLow(n, static_cast<uint16_t>(sum));
            result[n] = static_cast<uint16_t>(sum);
            vco = static_cast<uint16_t>(SetBit(vco, n, (sum >> 16) != 0));
        }
        else
        {
            // VSUBC: the unsigned difference, with the borrow and "not equal" in VCO.
            const uint32_t difference = us - ut;
            SetAccLow(n, static_cast<uint16_t>(difference));
            result[n] = static_cast<uint16_t>(difference);
            vco = static_cast<uint16_t>(SetBit(vco, n, (difference >> 16) != 0));
            vco = static_cast<uint16_t>(SetBit(vco, n + 8, difference != 0));
        }
    }

    // VABS leaves VCO alone. The others use its carries and clear it, or set new ones.
    if constexpr (kOp != 0x13)
    {
        vco_ = vco;
    }

    vr_[vd] = result;
}

template <uint32_t kOp>
void VectorUnit::Select(int vd, int vs, const Vector& vt)
{
    Vector result;
    uint32_t vcc = vcc_;
    uint32_t vco = vco_;
    uint32_t vce = vce_;
    for (int n = 0; n < 8; n++)
    {
        const uint16_t us = vr_[vs][n];
        const uint16_t ut = vt[n];
        const int32_t s = Signed(us);
        const int32_t t = Signed(ut);
        const bool carry = Bit(vco_, n);
        const bool not_equal = Bit(vco_, n + 8);
        if constexpr (kOp == 0x20)
        {
            // VLT.
            const bool less = s < t || (s == t && carry && not_equal);
            vcc = SetBit(vcc, n, less);
            result[n] = less ? us : ut;
        }
        else if constexpr (kOp == 0x21)
        {
            // VEQ.
            const bool equal = !not_equal && us == ut;
            vcc = SetBit(vcc, n, equal);
            result[n] = equal ? us : ut;
        }
        else if constexpr (kOp == 0x22)
        {
            // VNE.
            const bool different = us != ut || not_equal;
            vcc = SetBit(vcc, n, different);
            result[n] = different ? us : ut;
        }
        else if constexpr (kOp == 0x23)
        {
            // VGE.
            const bool greater = s > t || (s == t && (!carry || !not_equal));
            vcc = SetBit(vcc, n, greater);
            result[n] = greater ? us : ut;
        }
        else if constexpr (kOp == 0x24)
        {
            // VCL: the low half of a double-precision clip test, with the flags that VCH left.
            uint16_t value = 0;
            if (carry)
            {
                if (not_equal)
                {
                    value = Bit(vcc_, n) ? static_cast<uint16_t>(-ut) : us;
                }
                else
                {
                    const uint32_t sum32 = static_cast<uint32_t>(us) + ut;
                    const uint16_t sum = static_cast<uint16_t>(sum32);
                    const bool overflow = sum32 != sum;
                    const bool le = Bit(vce_, n) ? (sum == 0 || !overflow) : (sum == 0 && !overflow);
                    vcc = SetBit(vcc, n, le);
                    value = le ? static_cast<uint16_t>(-ut) : us;
                }
            }
            else
            {
                if (not_equal)
                {
                    value = Bit(vcc_, n + 8) ? ut : us;
                }
                else
                {
                    const bool ge = static_cast<int32_t>(us) - static_cast<int32_t>(ut) >= 0;
                    vcc = SetBit(vcc, n + 8, ge);
                    value = ge ? ut : us;
                }
            }

            result[n] = value;
        }
        else if constexpr (kOp == 0x25)
        {
            // VCH: the high half of a clip test, or a single-precision one.
            if ((s ^ t) < 0)
            {
                const int16_t sum = static_cast<int16_t>(s + t);
                const bool le = sum <= 0;
                vcc = SetBit(vcc, n, le);
                vcc = SetBit(vcc, n + 8, t < 0);
                vco = SetBit(vco, n, true);
                vco = SetBit(vco, n + 8, sum != 0 && us != static_cast<uint16_t>(ut ^ 0xFFFF));
                vce = SetBit(vce, n, sum == -1);
                result[n] = le ? static_cast<uint16_t>(-t) : us;
            }
            else
            {
                const int16_t difference = static_cast<int16_t>(s - t);
                const bool ge = difference >= 0;
                vcc = SetBit(vcc, n, t < 0);
                vcc = SetBit(vcc, n + 8, ge);
                vco = SetBit(vco, n, false);
                vco = SetBit(vco, n + 8, difference != 0 && us != static_cast<uint16_t>(ut ^ 0xFFFF));
                vce = SetBit(vce, n, false);
                result[n] = ge ? ut : us;
            }
        }
        else if constexpr (kOp == 0x26)
        {
            // VCR: a single-precision clip test in one's complement.
            if ((s ^ t) < 0)
            {
                vcc = SetBit(vcc, n + 8, t < 0);
                const bool le = s + t + 1 <= 0;
                vcc = SetBit(vcc, n, le);
                result[n] = le ? static_cast<uint16_t>(~ut) : us;
            }
            else
            {
                vcc = SetBit(vcc, n, t < 0);
                const bool ge = s - t >= 0;
                vcc = SetBit(vcc, n + 8, ge);
                result[n] = ge ? ut : us;
            }
        }
        else
        {
            // VMRG.
            result[n] = Bit(vcc_, n) ? us : ut;
        }

        SetAccLow(n, result[n]);
    }

    // The compares clear VCO and the clip bits of VCC. VCL and VCR clear VCE. VMRG clears VCO.
    if constexpr (kOp <= 0x23)
    {
        vcc &= 0x00FF;
        vco = 0;
    }
    else if constexpr (kOp == 0x24 || kOp == 0x26)
    {
        vco = 0;
        vce = 0;
    }
    else if constexpr (kOp == 0x27)
    {
        vco = 0;
    }

    vcc_ = static_cast<uint16_t>(vcc);
    vco_ = static_cast<uint16_t>(vco);
    vce_ = static_cast<uint8_t>(vce);
    vr_[vd] = result;
}

template <uint32_t kOp>
void VectorUnit::Logical(int vd, int vs, const Vector& vt)
{
    Vector result;
    for (int n = 0; n < 8; n++)
    {
        const uint16_t s = vr_[vs][n];
        const uint16_t t = vt[n];
        uint16_t value = 0;
        if constexpr (kOp == 0x28)
        {
            value = s & t;
        }
        else if constexpr (kOp == 0x29)
        {
            value = static_cast<uint16_t>(~(s & t));
        }
        else if constexpr (kOp == 0x2A)
        {
            value = s | t;
        }
        else if constexpr (kOp == 0x2B)
        {
            value = static_cast<uint16_t>(~(s | t));
        }
        else if constexpr (kOp == 0x2C)
        {
            value = s ^ t;
        }
        else
        {
            value = static_cast<uint16_t>(~(s ^ t));
        }

        result[n] = value;
        SetAccLow(n, value);
    }

    vr_[vd] = result;
}

void VectorUnit::Reserved(int vd, int vs, const Vector& vt)
{
    // The reserved opcodes add vs and vt into the accumulator's low slice and write zero.
    for (int n = 0; n < 8; n++)
    {
        const uint16_t sum = static_cast<uint16_t>(vr_[vs][n] + vt[n]);
        SetAccLow(n, sum);
        vr_[vd][n] = 0;
    }
}

int32_t VectorUnit::Reciprocal(int32_t input, bool square_root) const
{
    // The magnitude: two's complement for 16-bit inputs, one's complement below -32768.
    const int32_t mask = input >> 31;
    int32_t data = input ^ mask;
    if (input > -32768)
    {
        data -= mask;
    }

    if (data == 0)
    {
        return 0x7FFFFFFF;
    }
    if (input == -32768)
    {
        return static_cast<int32_t>(0xFFFF0000u);
    }

    const int shift = std::countl_zero(static_cast<uint32_t>(data));
    uint32_t index = ((static_cast<uint32_t>(data) << shift) & 0x7FC00000u) >> 22;
    int right_shift = 31 - shift;
    if (square_root)
    {
        index = 0x200 | (index & 0x1FE) | static_cast<uint32_t>(shift & 1);
        right_shift >>= 1;
    }

    const uint32_t result = (0x10000u | kDivideTable[index]) << 14;
    return static_cast<int32_t>(result >> right_shift) ^ mask;
}

template <uint32_t kOp>
void VectorUnit::Divide(VectorUnit& unit, int vd, int vs, int vt, int element)
{
    // The single-lane instructions read vt's lane (element & 7) and write vd's lane de. The accumulator's low slice
    // gets vt with the element's broadcast.
    const int de = vs & 7;
    const Vector vte = unit.Broadcast(vt, element);
    const uint16_t input = unit.vr_[vt][element & 7];
    if constexpr (kOp == 0x30 || kOp == 0x31 || kOp == 0x34 || kOp == 0x35)
    {
        const bool low = kOp == 0x31 || kOp == 0x35;
        const int32_t value = low && unit.div_double_
                                  ? static_cast<int32_t>((static_cast<uint32_t>(unit.div_in_) << 16) | input)
                                  : static_cast<int32_t>(Signed(input));
        const int32_t result = unit.Reciprocal(value, kOp >= 0x34);
        unit.div_double_ = false;
        unit.div_out_ = result >> 16;
        unit.vr_[vd][de] = static_cast<uint16_t>(result);
    }
    else if constexpr (kOp == 0x32 || kOp == 0x36)
    {
        // VRCPH and VRSQH: keep the high half of the input for the next VRCPL or VRSQL, and write the high half of the
        // last result.
        unit.div_double_ = true;
        unit.div_in_ = input;
        unit.vr_[vd][de] = static_cast<uint16_t>(unit.div_out_);
    }
    else
    {
        // VMOV.
        unit.vr_[vd][de] = vte[de];
    }

    for (int n = 0; n < 8; n++)
    {
        unit.SetAccLow(n, vte[n]);
    }
}

} // namespace usf::n64
