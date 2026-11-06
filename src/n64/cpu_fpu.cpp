// SPDX-License-Identifier: MIT

// The VR4300's COP1: the floating-point unit.
//
// Arithmetic runs on the host's IEEE 754 hardware in the rounding mode that FCR31 selects, and the host's exception
// flags give the cause bits. The VR4300 leaves some cases to software: a subnormal or quiet NaN operand, a subnormal
// result while FS is clear, and conversions out of range. Each raises the unimplemented operation exception. The
// VR4300's NaNs follow the old MIPS convention: a NaN with the top bit of its fraction set is signaling. Every NaN
// that an operation produces is the default NaN.
//
// This file must be compiled with strict floating-point semantics, so that the compiler keeps each operation between
// the calls that set the rounding mode and read the flags.

#include <bit>
#include <cfenv>
#include <cmath>
#include <cstdint>
#include <limits>

#include "n64/cpu.h"
#include "n64/system.h"

namespace usf::n64
{

namespace
{

// FCR31's fields. The flag, enable and cause fields hold the exceptions in this order: inexact, underflow, overflow,
// division by zero and invalid operation. The cause field has a sixth bit: unimplemented operation.
constexpr uint32_t kInexact = 1u << 0;
constexpr uint32_t kUnderflow = 1u << 1;
constexpr uint32_t kOverflow = 1u << 2;
constexpr uint32_t kDivideByZero = 1u << 3;
constexpr uint32_t kInvalid = 1u << 4;
constexpr uint32_t kUnimplemented = 1u << 5;
constexpr int kFlagShift = 2;
constexpr int kEnableShift = 7;
constexpr int kCauseShift = 12;
constexpr uint32_t kCauseField = 0x3Fu << kCauseShift;
constexpr uint32_t kCondition = 1u << 23;
constexpr uint32_t kFlushSubnormals = 1u << 24;
constexpr uint32_t kFcr31WriteMask = 0x0183FFFF;
constexpr uint32_t kFcr0 = 0x00000A00;

// FCR31's rounding modes.
constexpr int kRoundNearest = 0;
constexpr int kRoundZero = 1;
constexpr int kRoundUp = 2;
constexpr int kRoundDown = 3;

template <typename T>
struct FloatFormat;

template <>
struct FloatFormat<float>
{
    using Bits = uint32_t;
    static constexpr Bits kExponent = 0x7F800000;
    static constexpr Bits kFraction = 0x007FFFFF;
    static constexpr Bits kSignalingBit = 0x00400000;
    static constexpr Bits kDefaultNan = 0x7FBFFFFF;
};

template <>
struct FloatFormat<double>
{
    using Bits = uint64_t;
    static constexpr Bits kExponent = 0x7FF0000000000000ull;
    static constexpr Bits kFraction = 0x000FFFFFFFFFFFFFull;
    static constexpr Bits kSignalingBit = 0x0008000000000000ull;
    static constexpr Bits kDefaultNan = 0x7FF7FFFFFFFFFFFFull;
};

template <typename T>
bool IsNan(T value)
{
    using F = FloatFormat<T>;
    const auto bits = std::bit_cast<typename F::Bits>(value);
    return (bits & F::kExponent) == F::kExponent && (bits & F::kFraction) != 0;
}

template <typename T>
bool IsSignalingNan(T value)
{
    using F = FloatFormat<T>;
    const auto bits = std::bit_cast<typename F::Bits>(value);
    return IsNan(value) && (bits & F::kSignalingBit) != 0;
}

template <typename T>
bool IsQuietNan(T value)
{
    return IsNan(value) && !IsSignalingNan(value);
}

template <typename T>
bool IsSubnormal(T value)
{
    using F = FloatFormat<T>;
    const auto bits = std::bit_cast<typename F::Bits>(value);
    return (bits & F::kExponent) == 0 && (bits & F::kFraction) != 0;
}

template <typename T>
bool IsInfinite(T value)
{
    using F = FloatFormat<T>;
    const auto bits = std::bit_cast<typename F::Bits>(value);
    return (bits & F::kExponent) == F::kExponent && (bits & F::kFraction) == 0;
}

int HostRounding(int mode)
{
    switch (mode)
    {
    case kRoundZero:
        return FE_TOWARDZERO;
    case kRoundUp:
        return FE_UPWARD;
    case kRoundDown:
        return FE_DOWNWARD;
    case kRoundNearest:
    default:
        return FE_TONEAREST;
    }
}

// Runs `operation` on the host in the given rounding mode and returns its result. `flags` gets the host's exception
// flags that it raised.
template <typename R, typename F>
R HostCompute(bool traps, int mode, int& flags, F operation)
{
    // Without traps the flags have no effect. In the rounding mode that the host runs in, nothing needs setting.
    if (!traps && mode == kRoundNearest)
    {
        volatile R result = operation();
        return result;
    }

    const int saved = std::fegetround();
    std::fesetround(HostRounding(mode));
    std::feclearexcept(FE_ALL_EXCEPT);
    volatile R result = operation();
    flags = std::fetestexcept(FE_INEXACT | FE_UNDERFLOW | FE_OVERFLOW | FE_DIVBYZERO | FE_INVALID);
    std::feclearexcept(FE_ALL_EXCEPT);
    std::fesetround(saved);

    return result;
}

// Rounds to an integral value in one of FCR31's modes. Each step is exact.
template <typename T>
T RoundIntegral(T value, int mode)
{
    switch (mode)
    {
    case kRoundZero:
        return std::trunc(value);

    case kRoundUp:
        return std::ceil(value);

    case kRoundDown:
        return std::floor(value);

    default:
        {
            // To nearest, ties to even.
            const T truncated = std::trunc(value);
            const T fraction = std::fabs(value - truncated);
            const T step = std::copysign(T(1), value);
            if (fraction > T(0.5))
            {
                return truncated + step;
            }
            if (fraction == T(0.5) && std::fmod(truncated, T(2)) != 0)
            {
                return truncated + step;
            }

            return truncated;
        }
    }
}

template <typename T>
T ToFloat(uint64_t raw)
{
    if constexpr (sizeof(T) == 4)
    {
        return std::bit_cast<float>(static_cast<uint32_t>(raw));
    }
    else
    {
        return std::bit_cast<double>(raw);
    }
}

template <typename T>
uint64_t ToRaw(T value)
{
    return static_cast<uint64_t>(std::bit_cast<typename FloatFormat<T>::Bits>(value));
}

uint64_t SignExtend32(uint32_t value)
{
    return static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(value)));
}

} // namespace

uint64_t Cpu::Fpr(int index) const
{
    return fpr_[index & 31];
}

void Cpu::SetFpr(int index, uint64_t value)
{
    fpr_[index & 31] = value;
}

uint32_t Cpu::Fcr31() const
{
    return fcr31_;
}

void Cpu::SetFcr31(uint32_t value)
{
    // The legacy players keep all 32 bits. Their rounding mode stays as it was until the game writes FCR31.
    fcr31_ = FpuTraps() ? value & kFcr31WriteMask : value;
}

bool Cpu::RequireCop1()
{
    if ((status_ & kStatusCu1) == 0)
    {
        RaiseException(ExceptionCode::kCoprocessorUnusable, 1);
        return false;
    }

    return true;
}

uint32_t Cpu::MoveFromFpr32(int index) const
{
    if ((status_ & kStatusFr) != 0)
    {
        return static_cast<uint32_t>(fpr_[index]);
    }
    if ((index & 1) != 0)
    {
        return static_cast<uint32_t>(fpr_[index & ~1] >> 32);
    }

    return static_cast<uint32_t>(fpr_[index]);
}

void Cpu::MoveToFpr32(int index, uint32_t value)
{
    if ((status_ & kStatusFr) == 0 && (index & 1) != 0)
    {
        uint64_t& reg = fpr_[index & ~1];
        reg = (reg & 0xFFFFFFFFull) | (uint64_t{value} << 32);
        return;
    }

    uint64_t& reg = fpr_[index];
    reg = (reg & 0xFFFFFFFF00000000ull) | value;
}

uint64_t Cpu::MoveFromFpr64(int index) const
{
    return (status_ & kStatusFr) != 0 ? fpr_[index] : fpr_[index & ~1];
}

void Cpu::MoveToFpr64(int index, uint64_t value)
{
    if ((status_ & kStatusFr) != 0)
    {
        fpr_[index] = value;
    }
    else
    {
        fpr_[index & ~1] = value;
    }
}

uint32_t Cpu::SourceSingle(int index) const
{
    return static_cast<uint32_t>((status_ & kStatusFr) != 0 ? fpr_[index] : fpr_[index & ~1]);
}

uint64_t Cpu::SourceDouble(int index) const
{
    return (status_ & kStatusFr) != 0 ? fpr_[index] : fpr_[index & ~1];
}

void Cpu::WriteFcr31(uint32_t value)
{
    if (!FpuTraps())
    {
        fcr31_ = value;
        legacy_rounding_ = static_cast<int>(value & 3);
        return;
    }

    fcr31_ = value & kFcr31WriteMask;
    const uint32_t cause = (fcr31_ >> kCauseShift) & 0x3F;
    const uint32_t enabled = ((fcr31_ >> kEnableShift) & 0x1F) | kUnimplemented;
    if ((cause & enabled) != 0 && FpuTraps())
    {
        RaiseException(ExceptionCode::kFloatingPoint);
    }
}

bool Cpu::FpuStart()
{
    if (!RequireCop1())
    {
        return false;
    }

    if (FpuTraps())
    {
        fcr31_ &= ~kCauseField;
    }

    return true;
}

bool Cpu::FpuTraps() const
{
    return system_.GetOptions().fpu_traps;
}

int Cpu::FpuRounding() const
{
    return FpuTraps() ? static_cast<int>(fcr31_ & 3) : legacy_rounding_;
}

bool Cpu::FpuUnimplemented()
{
    // Without traps, the host's FPU computes the operation.
    if (!FpuTraps())
    {
        return true;
    }

    fcr31_ |= kUnimplemented << kCauseShift;
    RaiseException(ExceptionCode::kFloatingPoint);

    return false;
}

bool Cpu::FpuSignal(uint32_t exceptions)
{
    // The legacy players neither raise these exceptions nor record them.
    if (!FpuTraps())
    {
        return true;
    }

    const uint32_t enabled = (fcr31_ >> kEnableShift) & exceptions & 0x1F;
    fcr31_ |= exceptions << kCauseShift;
    fcr31_ |= (exceptions & ~enabled) << kFlagShift;
    if (enabled != 0)
    {
        RaiseException(ExceptionCode::kFloatingPoint);
        return false;
    }

    return true;
}

template <typename T>
bool Cpu::FpuCheckInput(T value)
{
    if (IsSubnormal(value))
    {
        return FpuUnimplemented();
    }
    if (IsNan(value))
    {
        return IsSignalingNan(value) ? FpuSignal(kInvalid) : FpuUnimplemented();
    }

    return true;
}

template <typename T>
bool Cpu::FpuCheckInputs(T a, T b)
{
    if (IsQuietNan(a) || IsQuietNan(b) || IsSubnormal(a) || IsSubnormal(b))
    {
        return FpuUnimplemented();
    }
    if (IsSignalingNan(a) || IsSignalingNan(b))
    {
        return FpuSignal(kInvalid);
    }

    return true;
}

bool Cpu::FpuSubnormalTraps() const
{
    // A subnormal result is left to software unless FS is set and neither underflow nor inexact traps.
    const uint32_t enables = (fcr31_ >> kEnableShift) & (kUnderflow | kInexact);
    return (fcr31_ & kFlushSubnormals) == 0 || enables != 0;
}

bool Cpu::FpuCheckHostFlags(int host_flags, bool conversion)
{
    if (host_flags == 0)
    {
        return true;
    }
    if (conversion && (host_flags & FE_INVALID) != 0)
    {
        return FpuUnimplemented();
    }
    if ((host_flags & FE_UNDERFLOW) != 0 && FpuSubnormalTraps() && FpuTraps())
    {
        return FpuUnimplemented();
    }

    uint32_t exceptions = 0;
    if ((host_flags & FE_INEXACT) != 0)
    {
        exceptions |= kInexact;
    }
    if ((host_flags & FE_UNDERFLOW) != 0)
    {
        exceptions |= kUnderflow;
    }
    if ((host_flags & FE_OVERFLOW) != 0)
    {
        exceptions |= kOverflow;
    }
    if ((host_flags & FE_DIVBYZERO) != 0)
    {
        exceptions |= kDivideByZero;
    }
    if ((host_flags & FE_INVALID) != 0)
    {
        exceptions |= kInvalid;
    }

    return FpuSignal(exceptions);
}

template <typename T>
bool Cpu::FpuCheckOutput(T& value)
{
    if (!FpuTraps())
    {
        // The host's FPU keeps a subnormal result and the bits of a NaN.
        return true;
    }

    if (IsSubnormal(value))
    {
        if (FpuSubnormalTraps())
        {
            return FpuUnimplemented();
        }

        // FS flushes the result: to zero, or to the smallest normal number when the rounding mode points away from
        // zero.
        FpuSignal(kUnderflow | kInexact);
        const bool negative = std::signbit(value);
        switch (fcr31_ & 3)
        {
        case kRoundUp:
            value = negative ? -T(0) : std::numeric_limits<T>::min();
            break;
        case kRoundDown:
            value = negative ? -std::numeric_limits<T>::min() : T(0);
            break;
        default:
            value = negative ? -T(0) : T(0);
            break;
        }

        return true;
    }

    if (IsNan(value))
    {
        value = ToFloat<T>(FloatFormat<T>::kDefaultNan);
    }

    return true;
}

template <typename T>
void Cpu::FpuCompare(uint32_t condition, T a, T b)
{
    // The condition's bits: 0 is the result when unordered, 1 tests equal, 2 tests less, and 3 makes every NaN signal
    // invalid. Without bit 3, only a signaling NaN does.
    bool result = false;
    if (IsNan(a) || IsNan(b))
    {
        const bool signaling = (condition & 8) != 0;
        if (IsNan(a) && (signaling || IsSignalingNan(a)) && !FpuSignal(kInvalid))
        {
            return;
        }
        if (IsNan(b) && (signaling || IsSignalingNan(b)) && !FpuSignal(kInvalid))
        {
            return;
        }

        result = (condition & 1) != 0;
    }
    else
    {
        result = ((condition & 2) != 0 && a == b) || ((condition & 4) != 0 && a < b);
    }

    if (result)
    {
        fcr31_ |= kCondition;
    }
    else
    {
        fcr31_ &= ~kCondition;
    }
}

template <typename T>
void Cpu::FpuConvertToInteger(uint32_t instruction, T value, bool doubleword, int rounding)
{
    const int fd = static_cast<int>(Sa(instruction));
    if (!FpuTraps())
    {
        // The legacy players convert on the host. A NaN and a value that doesn't fit give what x86 hosts give: the
        // most negative integer.
        const T rounded = RoundIntegral(value, rounding);
        const T limit = doubleword ? T(9223372036854775808.0) : T(2147483648.0);
        const bool fits = !IsNan(value) && rounded < limit && rounded >= -limit;
        if (doubleword)
        {
            fpr_[fd] = fits ? static_cast<uint64_t>(static_cast<int64_t>(rounded)) : uint64_t{1} << 63;
        }
        else
        {
            fpr_[fd] = fits ? static_cast<uint32_t>(static_cast<int32_t>(rounded)) : uint32_t{1} << 31;
        }

        return;
    }

    if (IsSubnormal(value) || IsInfinite(value) || IsNan(value))
    {
        FpuUnimplemented();
        return;
    }

    // Values that the hardware can't convert: beyond 32 bits for a word, and 53 bits for a doubleword.
    if (doubleword)
    {
        const T limit = T(9007199254740992.0);
        if (value >= limit || value <= -limit)
        {
            FpuUnimplemented();
            return;
        }
    }
    else
    {
        const T limit = T(2147483648.0);
        if (value >= limit || value < -limit)
        {
            FpuUnimplemented();
            return;
        }
    }

    const T rounded = RoundIntegral(value, rounding);
    if (!doubleword && (rounded >= T(2147483648.0) || rounded < T(-2147483648.0)))
    {
        FpuUnimplemented();
        return;
    }
    if (rounded != value && !FpuSignal(kInexact))
    {
        return;
    }

    if (doubleword)
    {
        fpr_[fd] = static_cast<uint64_t>(static_cast<int64_t>(rounded));
    }
    else
    {
        fpr_[fd] = static_cast<uint32_t>(static_cast<int32_t>(rounded));
    }
}

template <typename T>
void Cpu::ExecuteFpuFormat(uint32_t instruction)
{
    const int fs = static_cast<int>(Rd(instruction));
    const int ft = static_cast<int>(Rt(instruction));
    const int fd = static_cast<int>(Sa(instruction));
    const uint32_t funct = instruction & 0x3F;
    const int mode = FpuRounding();
    constexpr bool kSingle = sizeof(T) == 4;

    // fs comes from the even register of a pair when FR is clear. ft and fd are the registers named.
    auto source = [this](int index)
    {
        return ToFloat<T>(kSingle ? SourceSingle(index) : SourceDouble(index));
    };
    auto target = [this](int index)
    {
        return ToFloat<T>(kSingle ? static_cast<uint32_t>(fpr_[index]) : fpr_[index]);
    };
    auto write = [this, fd](auto result)
    {
        fpr_[fd] = ToRaw(result);
    };

    if (funct == 0x06)
    {
        // MOV copies the raw 64 bits and changes no cause bits.
        if (RequireCop1())
        {
            fpr_[fd] = SourceDouble(fs);
        }

        return;
    }

    if (!FpuStart())
    {
        return;
    }

    int flags = 0;
    switch (funct)
    {
    case 0x00:
    case 0x01:
    case 0x02:
    case 0x03:
        {
            const T a = source(fs);
            const T b = target(ft);
            if (!FpuCheckInputs(a, b))
            {
                return;
            }

            T result = HostCompute<T>(FpuTraps(), mode, flags,
                                      [a, b, funct]
                                      {
                                          switch (funct)
                                          {
                                          case 0x00:
                                              return a + b;
                                          case 0x01:
                                              return a - b;
                                          case 0x02:
                                              return a * b;
                                          default:
                                              return a / b;
                                          }
                                      });
            if (FpuCheckHostFlags(flags, false) && FpuCheckOutput(result))
            {
                write(result);
            }
            return;
        }

    case 0x04:
    case 0x05:
    case 0x07:
        {
            const T a = source(fs);
            if (!FpuCheckInput(a))
            {
                return;
            }

            T result = HostCompute<T>(FpuTraps(), mode, flags,
                                      [a, funct]
                                      {
                                          if (funct == 0x04)
                                          {
                                              return std::sqrt(a);
                                          }

                                          return funct == 0x05 ? std::fabs(a) : -a;
                                      });
            if (FpuCheckHostFlags(flags, false) && FpuCheckOutput(result))
            {
                write(result);
            }
            return;
        }

    case 0x08:
    case 0x09:
    case 0x0A:
    case 0x0B:
        // ROUND.L, TRUNC.L, CEIL.L and FLOOR.L.
        FpuConvertToInteger(instruction, source(fs), true, static_cast<int>(funct & 3));
        return;

    case 0x0C:
    case 0x0D:
    case 0x0E:
    case 0x0F:
        FpuConvertToInteger(instruction, source(fs), false, static_cast<int>(funct & 3));
        return;

    case 0x20:
    case 0x21:
        {
            // CVT.S and CVT.D. Converting a format to itself is left to software.
            const bool to_single = funct == 0x20;
            if (to_single == kSingle)
            {
                FpuUnimplemented();
                return;
            }

            const T a = source(fs);
            if (!FpuCheckInput(a))
            {
                return;
            }

            if (to_single)
            {
                float result = HostCompute<float>(FpuTraps(), mode, flags, [a] { return static_cast<float>(a); });
                if (FpuCheckHostFlags(flags, false) && FpuCheckOutput(result))
                {
                    write(result);
                }
            }
            else
            {
                double result = HostCompute<double>(FpuTraps(), mode, flags, [a] { return static_cast<double>(a); });
                if (FpuCheckHostFlags(flags, false) && FpuCheckOutput(result))
                {
                    write(result);
                }
            }
            return;
        }

    case 0x24:
    case 0x25:
        // CVT.W and CVT.L. The legacy players take the rounding mode of these two from FCR31 itself, even before the
        // game writes FCR31.
        FpuConvertToInteger(instruction, source(fs), funct == 0x25, static_cast<int>(fcr31_ & 3));
        return;

    default:
        if (funct >= 0x30)
        {
            FpuCompare(funct & 0xF, source(fs), target(ft));
            return;
        }

        FpuUnimplemented();
        return;
    }
}

void Cpu::ExecuteFpuInteger(uint32_t instruction, bool doubleword)
{
    const int fs = static_cast<int>(Rd(instruction));
    const int fd = static_cast<int>(Sa(instruction));
    const uint32_t funct = instruction & 0x3F;
    const int mode = FpuRounding();

    if (!FpuStart())
    {
        return;
    }
    if (funct != 0x20 && funct != 0x21)
    {
        FpuUnimplemented();
        return;
    }

    // CVT.S and CVT.D from a word or a doubleword. Doublewords beyond 55 bits are left to software.
    const int64_t value = doubleword ? static_cast<int64_t>(SourceDouble(fs))
                                     : static_cast<int64_t>(static_cast<int32_t>(SourceSingle(fs)));
    if (doubleword && (value >= (int64_t{1} << 55) || value < -(int64_t{1} << 55)) && !FpuUnimplemented())
    {
        return;
    }

    int flags = 0;
    if (funct == 0x20)
    {
        float result = HostCompute<float>(FpuTraps(), mode, flags, [value] { return static_cast<float>(value); });
        if (FpuCheckHostFlags(flags, false) && FpuCheckOutput(result))
        {
            fpr_[fd] = ToRaw(result);
        }
    }
    else
    {
        double result = HostCompute<double>(FpuTraps(), mode, flags, [value] { return static_cast<double>(value); });
        if (FpuCheckHostFlags(flags, false) && FpuCheckOutput(result))
        {
            fpr_[fd] = ToRaw(result);
        }
    }
}

void Cpu::ExecuteCop1(uint32_t instruction)
{
    const uint32_t op = instruction >> 26;
    const uint32_t rt = Rt(instruction);
    if (op != 0x11)
    {
        // LWC1, LDC1, SWC1 and SDC1.
        if (!RequireCop1())
        {
            return;
        }

        const uint64_t vaddr = gpr_[Rs(instruction)] + SignedImmediate(instruction);
        const int ft = static_cast<int>(rt);
        switch (op)
        {
        case 0x31:
            {
                uint32_t value = 0;
                if (Load(vaddr, value))
                {
                    MoveToFpr32(ft, value);
                }
                break;
            }

        case 0x35:
            {
                uint64_t value = 0;
                if (Load(vaddr, value))
                {
                    MoveToFpr64(ft, value);
                }
                break;
            }

        case 0x39:
            Store<uint32_t>(vaddr, MoveFromFpr32(ft));
            break;

        default:
            Store<uint64_t>(vaddr, MoveFromFpr64(ft));
            break;
        }

        return;
    }

    const uint32_t rs = Rs(instruction);
    const int fs = static_cast<int>(Rd(instruction));
    switch (rs)
    {
    case 0x00:
        if (RequireCop1())
        {
            gpr_[rt] = SignExtend32(MoveFromFpr32(fs));
        }
        return;

    case 0x01:
        if (RequireCop1())
        {
            gpr_[rt] = MoveFromFpr64(fs);
        }
        return;

    case 0x02:
        if (RequireCop1())
        {
            uint32_t value = 0;
            if (fs == 0)
            {
                value = kFcr0;
            }
            else if (fs == 31)
            {
                value = fcr31_;
            }

            gpr_[rt] = SignExtend32(value);
        }
        return;

    case 0x04:
        if (RequireCop1())
        {
            MoveToFpr32(fs, static_cast<uint32_t>(gpr_[rt]));
        }
        return;

    case 0x05:
        if (RequireCop1())
        {
            MoveToFpr64(fs, gpr_[rt]);
        }
        return;

    case 0x06:
        if (RequireCop1() && fs == 31)
        {
            WriteFcr31(static_cast<uint32_t>(gpr_[rt]));
        }
        return;

    case 0x08:
        {
            // BC1F, BC1T, BC1FL and BC1TL.
            if (!FpuStart())
            {
                return;
            }
            if (rt >= 4)
            {
                FpuUnimplemented();
                return;
            }

            const bool condition = (fcr31_ & kCondition) != 0;
            const bool taken = condition == ((rt & 1) != 0);
            if ((rt & 2) != 0)
            {
                BranchLikely(taken, BranchTarget(instruction));
            }
            else
            {
                Branch(taken, BranchTarget(instruction));
            }
            return;
        }

    case 0x10:
        ExecuteFpuFormat<float>(instruction);
        return;

    case 0x11:
        ExecuteFpuFormat<double>(instruction);
        return;

    case 0x14:
        ExecuteFpuInteger(instruction, false);
        return;

    case 0x15:
        ExecuteFpuInteger(instruction, true);
        return;

    default:
        // DCFC1, DCTC1 and the formats that don't exist.
        if (FpuStart())
        {
            FpuUnimplemented();
        }
        return;
    }
}

} // namespace usf::n64
