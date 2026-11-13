// SPDX-License-Identifier: MIT

#include "rip/driver.h"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

#include "xsflib/text.h"

#include "n64/constants.h"
#include "rip/mips.h"

namespace usf::rip
{
namespace
{

using mips::Assembler;

// VI_CURRENT through KSEG1: the half-line that the VI is drawing. It falls back when a new field starts.
constexpr uint32_t kViBase = n64::kKseg1 + n64::kViRegisters;
constexpr uint16_t kViCurrentOffset = 0x10;

// The field wait loads the VI's base with a LUI alone.
static_assert((kViBase & 0xFFFF) == 0);

// The driver's stack frame: the 16 bytes of argument space that a callee may use, rounded up to 8-byte alignment.
constexpr uint16_t kFrameSize = 32;

// The flags of libultra's osRecvMesg: OS_MESG_NOBLOCK and OS_MESG_BLOCK.
constexpr uint32_t kMesgNoBlock = 0;
constexpr uint32_t kMesgBlock = 1;

void CheckAddress(uint32_t address, const char* what)
{
    if (address < n64::kKseg0 || address >= n64::kKseg1 || (address & 3) != 0)
    {
        throw std::invalid_argument(std::string(what) + " must be a word-aligned address in KSEG0");
    }
}

// Emits the conditions of a call. Each condition is a branch past the call. A frame call (`in_frame`) may be limited to
// a range of frames. s1 counts the frames. Returns the positions of the branches. Their targets are filled in once the
// call is emitted.
std::vector<size_t> EmitSkips(Assembler& code, const GameCall& call, bool in_frame)
{
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

    return skips;
}

// Emits the loading of one argument of a call into `reg`.
void EmitArgument(Assembler& code, const CallArgument& argument, mips::Register reg)
{
    if (argument.result)
    {
        code.Emit(mips::Or(reg, mips::kV0, mips::kZero));
        return;
    }
    if (!argument.track)
    {
        code.LoadImmediate(reg, argument.value);
        return;
    }

    if (argument.shift < 0 || argument.shift > 31 || argument.right_shift < 0 || argument.right_shift > 31)
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

    const std::vector<size_t> skips = EmitSkips(code, call, in_frame);
    for (size_t i = 0; i < call.arguments.size(); i++)
    {
        EmitArgument(code, call.arguments[i], static_cast<mips::Register>(mips::kA0 + i));
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

    const uint32_t end = options.address + static_cast<uint32_t>(4 * driver.code.size());
    if (options.end != 0 && end > options.end)
    {
        throw std::invalid_argument("the driver at " + xsflib::Hex(options.address, 8, true) + " runs to " +
                                    xsflib::Hex(end, 8, true) + ", past its end at " +
                                    xsflib::Hex(options.end, 8, true));
    }

    return driver;
}

GameCall MessageWait(uint32_t recv_mesg, uint32_t queue)
{
    return {.function = recv_mesg, .arguments = {{.value = queue}, {.value = 0}, {.value = kMesgBlock}}};
}

GameCall MessageDrop(uint32_t recv_mesg, uint32_t queue)
{
    return {.function = recv_mesg, .arguments = {{.value = queue}, {.value = 0}, {.value = kMesgNoBlock}}};
}

std::vector<GameCall> AddRetraceClient(const RetraceClient& client)
{
    // The queue's size in libultra's OSMesgQueue, and its one message.
    constexpr uint32_t kQueueSize = 24;
    constexpr uint32_t kMessages = 1;

    CheckAddress(client.address, "a retrace client");
    const uint32_t buffer = client.address + kQueueSize;
    const uint32_t record = buffer + 4 * kMessages;

    return {{.function = client.create_queue,
             .arguments = {{.value = client.address}, {.value = buffer}, {.value = kMessages}}},
            {.function = client.add_client,
             .arguments = {{.value = client.manager}, {.value = record}, {.value = client.address}}}};
}

RdramPatch DriverWordPatch(const Driver& driver, uint32_t word, uint32_t replacement)
{
    if (std::ranges::count(driver.code, word) != 1)
    {
        throw std::logic_error("the word to patch isn't in the driver's code exactly once");
    }

    const auto found = std::ranges::find(driver.code, word);
    return {.address = driver.address + static_cast<uint32_t>(4 * (found - driver.code.begin())),
            .words = {replacement}};
}

} // namespace usf::rip
