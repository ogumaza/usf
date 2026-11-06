// SPDX-License-Identifier: MIT

#include "rip/driver.h"

#include <stdexcept>
#include <string>
#include <vector>

#include "rip/mips.h"

namespace usf::rip
{

namespace
{

using mips::Assembler;

// VI_CURRENT through KSEG1: the half-line that the VI is drawing. It falls back when a new field starts.
constexpr uint32_t kViBase = 0xA4400000;
constexpr uint16_t kViCurrentOffset = 0x10;

// The driver's stack frame: the 16 bytes of argument space that a callee may use, rounded up to 8-byte alignment.
constexpr uint16_t kFrameSize = 32;

void CheckAddress(uint32_t address, const char* what)
{
    if (address < 0x80000000 || address >= 0xA0000000 || (address & 3) != 0)
    {
        throw std::invalid_argument(std::string(what) + " must be a word-aligned address in KSEG0");
    }
}

// Emits a call. A frame call (`in_frame`) may be limited to a range of frames, which s1 counts.
void EmitCall(Assembler& code, const GameCall& call, bool in_frame)
{
    if (call.arguments.size() > 4)
    {
        throw std::invalid_argument("a driver's call takes at most four arguments");
    }
    if ((call.function & 3) != 0)
    {
        throw std::invalid_argument("a driver's function must be word-aligned");
    }

    // The conditions branch past the call. Their targets are filled in once the call is emitted.
    std::vector<size_t> skips;
    if (call.track_mask != 0 || call.track_value != 0)
    {
        code.LoadImmediate(mips::kT0, call.track_mask);
        code.Emit(mips::And(mips::kT1, mips::kS0, mips::kT0));
        code.LoadImmediate(mips::kT0, call.track_value);
        skips.push_back(code.Size());
        code.Bne(mips::kT1, mips::kT0, code.Here());
        code.Emit(mips::kNop);
    }
    if (in_frame && call.frames != 0)
    {
        code.LoadImmediate(mips::kT0, call.frames);
        code.Emit(mips::Sltu(mips::kT1, mips::kS1, mips::kT0));
        skips.push_back(code.Size());
        code.Beq(mips::kT1, mips::kZero, code.Here());
        code.Emit(mips::kNop);
    }
    if (in_frame && call.first_frame != 0)
    {
        code.LoadImmediate(mips::kT0, call.first_frame);
        code.Emit(mips::Sltu(mips::kT1, mips::kS1, mips::kT0));
        skips.push_back(code.Size());
        code.Bne(mips::kT1, mips::kZero, code.Here());
        code.Emit(mips::kNop);
    }

    for (size_t i = 0; i < call.arguments.size(); i++)
    {
        const CallArgument& argument = call.arguments[i];
        const auto reg = static_cast<mips::Register>(mips::kA0 + i);
        if (argument.result)
        {
            code.Emit(mips::Or(reg, mips::kV0, mips::kZero));
            continue;
        }
        if (!argument.track)
        {
            code.LoadImmediate(reg, argument.value);
            continue;
        }

        if (argument.shift < 0 || argument.shift > 31)
        {
            throw std::invalid_argument("a driver's argument shifts by 0 to 31 bits");
        }
        if (argument.right_shift < 0 || argument.right_shift > 31)
        {
            throw std::invalid_argument("a driver's argument shifts by 0 to 31 bits");
        }

        if (argument.right_shift == 0 && argument.mask == 0)
        {
            code.Emit(mips::Sll(reg, mips::kS0, static_cast<uint32_t>(argument.shift)));
        }
        else
        {
            code.Emit(mips::Srl(reg, mips::kS0, static_cast<uint32_t>(argument.right_shift)));
            if (argument.mask != 0)
            {
                code.Emit(mips::Andi(reg, reg, argument.mask));
            }
            code.Emit(mips::Sll(reg, reg, static_cast<uint32_t>(argument.shift)));
        }

        if (argument.value != 0)
        {
            code.LoadImmediate(mips::kAt, argument.value);
            code.Emit(mips::Or(reg, reg, mips::kAt));
        }
    }

    code.LoadImmediate(mips::kT9, call.function);
    code.Emit(mips::Jalr(mips::kRa, mips::kT9));
    code.Emit(mips::kNop);

    for (size_t skip : skips)
    {
        code.SetBranchTarget(skip, code.Here());
    }
}

// Waits until VI_CURRENT falls back: the start of the next field.
void EmitFieldWait(Assembler& code)
{
    code.Emit(mips::Lw(mips::kT1, mips::kT0, kViCurrentOffset));
    const uint32_t poll = code.Here();
    code.Emit(mips::Or(mips::kT3, mips::kT1, mips::kZero));
    code.Emit(mips::Lw(mips::kT1, mips::kT0, kViCurrentOffset));
    code.Emit(mips::Sltu(mips::kT2, mips::kT1, mips::kT3));
    code.Beq(mips::kT2, mips::kZero, poll);
    code.Emit(mips::kNop);
}

} // namespace

Driver BuildCallDriver(const CallDriverOptions& options)
{
    CheckAddress(options.address, "a driver");
    if (options.fields < 1)
    {
        throw std::invalid_argument("a driver's frame lasts at least one field");
    }

    // The code reads the track word at its end, whose address is known only once the code is built. The first pass
    // finds the code's length. The second builds it with the right address.
    Driver driver;
    driver.address = options.address;
    uint32_t track_word = options.address;
    for (int pass = 0; pass < 2; pass++)
    {
        Assembler code(options.address);
        code.Emit(mips::Addiu(mips::kSp, mips::kSp, static_cast<uint16_t>(-kFrameSize)));
        code.Emit(mips::Lui(mips::kS1, mips::High(track_word)));
        code.Emit(mips::Lw(mips::kS0, mips::kS1, mips::Low(track_word)));

        for (const GameCall& call : options.start_calls)
        {
            EmitCall(code, call, false);
        }

        code.Emit(mips::Or(mips::kS1, mips::kZero, mips::kZero));

        // Without anything to do each frame, the driver ends in an idle loop: a jump to itself with a delay slot that
        // does nothing.
        const uint32_t loop = code.Here();
        if (options.frame_calls.empty() && !options.wait_call)
        {
            code.J(loop);
            code.Emit(mips::kNop);
        }
        else
        {
            for (const GameCall& call : options.frame_calls)
            {
                EmitCall(code, call, true);
            }

            if (!options.wait_call)
            {
                code.Emit(mips::Lui(mips::kT0, static_cast<uint16_t>(kViBase >> 16)));
            }
            for (int field = 0; field < options.fields; field++)
            {
                if (options.wait_call)
                {
                    EmitCall(code, *options.wait_call, false);
                }
                else
                {
                    EmitFieldWait(code);
                }
            }

            code.Emit(mips::Addiu(mips::kS1, mips::kS1, 1));
            code.J(loop);
            code.Emit(mips::kNop);
        }

        track_word = code.Here();
        code.Emit(0);
        driver.code = code.Words();
    }

    driver.track_word = track_word;

    return driver;
}

} // namespace usf::rip
