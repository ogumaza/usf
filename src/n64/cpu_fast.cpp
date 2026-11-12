// SPDX-License-Identifier: MIT

// The legacy timing's short path. RunFast runs the instructions that games run most from a table of decoded
// instructions, with the program counter, the clock and the delay slot in local variables. Anything else returns to
// RunLate, which runs that instruction through Step. The two paths give the same result. Both follow the rules of the
// legacy timing.
//
// Each table entry counts only while its `word` equals the word in RDRAM. No write to RDRAM has to tell the table.
//
// Games often wait in a short loop until an event changes memory. Such a loop changes nothing after its first pass.
// Only an event can change that, since the RSP's tasks and the DMAs run in the event handlers. RunFast therefore moves
// the clock to the end of the pass at which the next event is due. The pass count, Count and Random come out as they
// would if the loop had run.
//
// A poll of VI_CURRENT is such a wait too. A game or a rip's driver may spin on it for a whole field. The register runs
// here, and its value changes only at a known time. The clock therefore moves up to that time, one half-line at a time.

#include <algorithm>

#include "xsflib/bytes.h"

#include "n64/bits.h"
#include "n64/constants.h"
#include "n64/cpu.h"
#include "n64/cpu_cop0.h"
#include "n64/mips_registers.h"
#include "n64/system.h"

// Forces a lambda inline. The macro goes after the lambda's parameter list. A compiler that supports neither attribute
// gets none.
#if defined(__GNUC__) || defined(__clang__)
#define USF_INLINE_LAMBDA __attribute__((always_inline))
#elif defined(_MSC_VER)
#define USF_INLINE_LAMBDA [[msvc::forceinline]]
#else
#define USF_INLINE_LAMBDA
#endif

namespace usf::n64
{
namespace
{

constexpr uint32_t kNoBranch = ~0u;

// The longest wait loop, from its first instruction to its branch.
constexpr uint32_t kMaxWaitDistance = 6;

// A wait's registers can take a pass or two to settle. After this many passes that change them, a loop isn't a wait.
constexpr int kMaxWaitChanges = 4;

// VI_CURRENT's address in KSEG1.
constexpr uint64_t kViCurrentKseg1 = SignExtend32(kKseg1 + kViCurrent);

// A kernel-mode address with 32 bits. Returns false for an address that has to go through Translate.
inline bool Direct(uint64_t vaddr, uint32_t& paddr)
{
    const uint32_t address = static_cast<uint32_t>(vaddr);
    paddr = address & kPhysicalAddressMask;

    return (address & kSegmentMask) == kKseg0 && SignExtend32(address) == vaddr;
}

} // namespace

Cpu::FastInstruction Cpu::DecodeFast(uint32_t instruction)
{
    FastInstruction decoded;
    decoded.word = instruction;
    decoded.op = FastOp::kOther;
    decoded.rs = static_cast<uint8_t>(Rs(instruction));
    decoded.rt = static_cast<uint8_t>(Rt(instruction));
    decoded.rd = static_cast<uint8_t>(Rd(instruction));
    decoded.immediate = static_cast<int32_t>(static_cast<int16_t>(instruction));

    // An operation with register 0 as its destination does nothing. A load can still fault, so it stays with Step.
    const auto set_register = [&decoded](FastOp op)
    {
        decoded.op = decoded.rd == 0 ? FastOp::kNop : op;
    };
    const auto set_shift = [&decoded, instruction](FastOp op)
    {
        decoded.op = decoded.rd == 0 ? FastOp::kNop : op;
        decoded.immediate = static_cast<int32_t>(Sa(instruction));
    };
    const auto set_immediate = [&decoded](FastOp op)
    {
        decoded.op = decoded.rt == 0 ? FastOp::kNop : op;
    };
    const auto set_zero_extended = [&decoded, instruction](FastOp op)
    {
        decoded.op = decoded.rt == 0 ? FastOp::kNop : op;
        decoded.immediate = static_cast<int32_t>(instruction & 0xFFFF);
    };
    const auto set_load = [&decoded](FastOp op)
    {
        decoded.op = decoded.rt == 0 ? FastOp::kOther : op;
    };

    const auto set_branch = [&decoded](FastOp op)
    {
        decoded.op = op;
        decoded.immediate *= 4;
    };

    switch (instruction >> 26)
    {
    case 0x00:
        switch (instruction & 0x3F)
        {
        case 0x00:
            set_shift(FastOp::kSll);
            break;

        case 0x02:
            set_shift(FastOp::kSrl);
            break;

        case 0x03:
            set_shift(FastOp::kSra);
            break;

        case 0x04:
            set_register(FastOp::kSllv);
            break;

        case 0x06:
            set_register(FastOp::kSrlv);
            break;

        case 0x07:
            set_register(FastOp::kSrav);
            break;

        case 0x08:
            decoded.op = FastOp::kJr;
            break;

        case 0x09:
            decoded.op = decoded.rd == 0 ? FastOp::kOther : FastOp::kJalr;
            break;

        case 0x10:
            set_register(FastOp::kMfhi);
            break;

        case 0x12:
            set_register(FastOp::kMflo);
            break;

        case 0x18:
            decoded.op = FastOp::kMult;
            break;

        case 0x19:
            decoded.op = FastOp::kMultu;
            break;

        case 0x21:
            set_register(FastOp::kAddu);
            break;

        case 0x23:
            set_register(FastOp::kSubu);
            break;

        case 0x24:
            set_register(FastOp::kAnd);
            break;

        case 0x25:
            set_register(FastOp::kOr);
            break;

        case 0x26:
            set_register(FastOp::kXor);
            break;

        case 0x27:
            set_register(FastOp::kNor);
            break;

        case 0x2A:
            set_register(FastOp::kSlt);
            break;

        case 0x2B:
            set_register(FastOp::kSltu);
            break;

        case 0x2D:
            set_register(FastOp::kDaddu);
            break;

        case 0x2F:
            set_register(FastOp::kDsubu);
            break;

        case 0x38:
            set_shift(FastOp::kDsll);
            break;

        case 0x3A:
            set_shift(FastOp::kDsrl);
            break;

        case 0x3B:
            set_shift(FastOp::kDsra);
            break;

        case 0x3C:
            set_shift(FastOp::kDsll);
            decoded.immediate += 32;
            break;

        case 0x3E:
            set_shift(FastOp::kDsrl);
            decoded.immediate += 32;
            break;

        case 0x3F:
            set_shift(FastOp::kDsra);
            decoded.immediate += 32;
            break;

        default:
            break;
        }
        break;

    case 0x01:
        switch (decoded.rt)
        {
        case 0x00:
            set_branch(FastOp::kBltz);
            break;
        case 0x01:
            set_branch(FastOp::kBgez);
            break;
        case 0x02:
            set_branch(FastOp::kBltzl);
            break;
        case 0x03:
            set_branch(FastOp::kBgezl);
            break;
        default:
            break;
        }
        break;

    case 0x02:
    case 0x03:
        decoded.op = (instruction >> 26) == 0x02 ? FastOp::kJ : FastOp::kJal;
        decoded.immediate = static_cast<int32_t>((instruction & mips::kJumpIndexMask) << 2);
        break;

    case 0x04:
        set_branch(FastOp::kBeq);
        break;

    case 0x05:
        set_branch(FastOp::kBne);
        break;

    case 0x06:
        set_branch(FastOp::kBlez);
        break;

    case 0x07:
        set_branch(FastOp::kBgtz);
        break;

    case 0x09:
        set_immediate(FastOp::kAddiu);
        break;

    case 0x0A:
        set_immediate(FastOp::kSlti);
        break;

    case 0x0B:
        set_immediate(FastOp::kSltiu);
        break;

    case 0x0C:
        set_zero_extended(FastOp::kAndi);
        break;

    case 0x0D:
        set_zero_extended(FastOp::kOri);
        break;

    case 0x0E:
        set_zero_extended(FastOp::kXori);
        break;

    case 0x0F:
        set_immediate(FastOp::kLui);
        decoded.immediate = static_cast<int32_t>(instruction << 16);
        break;

    case 0x11:
        switch (decoded.rs)
        {
        case 0x00:
            set_load(FastOp::kMfc1);
            break;

        case 0x04:
            decoded.op = FastOp::kMtc1;
            break;

        case 0x08:
            if (decoded.rt < 4)
            {
                set_branch(FastOp::kBc1);
            }
            break;

        default:
            break;
        }
        break;

    case 0x14:
        set_branch(FastOp::kBeql);
        break;

    case 0x15:
        set_branch(FastOp::kBnel);
        break;

    case 0x16:
        set_branch(FastOp::kBlezl);
        break;

    case 0x17:
        set_branch(FastOp::kBgtzl);
        break;

    case 0x19:
        set_immediate(FastOp::kDaddiu);
        break;

    case 0x20:
        set_load(FastOp::kLb);
        break;

    case 0x21:
        set_load(FastOp::kLh);
        break;

    case 0x23:
        set_load(FastOp::kLw);
        break;

    case 0x24:
        set_load(FastOp::kLbu);
        break;

    case 0x25:
        set_load(FastOp::kLhu);
        break;

    case 0x28:
        decoded.op = FastOp::kSb;
        break;

    case 0x29:
        decoded.op = FastOp::kSh;
        break;

    case 0x2B:
        decoded.op = FastOp::kSw;
        break;

    case 0x2F:
        // CACHE: no cache is modelled, and RunFast runs only in kernel mode.
        decoded.op = FastOp::kNop;
        break;

    case 0x31:
        decoded.op = FastOp::kLwc1;
        break;

    case 0x35:
        decoded.op = FastOp::kLdc1;
        break;

    case 0x37:
        set_load(FastOp::kLd);
        break;

    case 0x39:
        decoded.op = FastOp::kSwc1;
        break;

    case 0x3D:
        decoded.op = FastOp::kSdc1;
        break;

    case 0x3F:
        decoded.op = FastOp::kSd;
        break;

    default:
        break;
    }

    return decoded;
}

Cpu::FastPage& Cpu::FastPageAt(uint32_t paddr)
{
    const size_t page = paddr >> kPageBits;
    if (page >= fast_pages_.size())
    {
        fast_pages_.resize(page + 1);
    }
    if (!fast_pages_[page])
    {
        fast_pages_[page] = std::make_unique<FastPage>();
    }

    return *fast_pages_[page];
}

int Cpu::WaitLoopRegisters(const FastInstruction* first, uint32_t count, std::array<uint8_t, kMaxWaitRegisters>& out)
{
    int found = 0;
    for (uint32_t i = 0; i < count; i++)
    {
        const FastInstruction& instruction = first[i];
        int destination = -1;
        switch (instruction.op)
        {
        case FastOp::kNop:
            break;

        case FastOp::kSll:
        case FastOp::kSrl:
        case FastOp::kSra:
        case FastOp::kSllv:
        case FastOp::kSrlv:
        case FastOp::kSrav:
        case FastOp::kMfhi:
        case FastOp::kMflo:
        case FastOp::kAddu:
        case FastOp::kSubu:
        case FastOp::kAnd:
        case FastOp::kOr:
        case FastOp::kXor:
        case FastOp::kNor:
        case FastOp::kSlt:
        case FastOp::kSltu:
            destination = instruction.rd;
            break;

        case FastOp::kAddiu:
        case FastOp::kSlti:
        case FastOp::kSltiu:
        case FastOp::kAndi:
        case FastOp::kOri:
        case FastOp::kXori:
        case FastOp::kLui:
        case FastOp::kLb:
        case FastOp::kLh:
        case FastOp::kLw:
        case FastOp::kLbu:
        case FastOp::kLhu:
            destination = instruction.rt;
            break;

        case FastOp::kSb:
        case FastOp::kSh:
        case FastOp::kSw:
            // Only a branch to itself may store, in its delay slot. It writes the same value to the same place on each
            // pass. A longer loop might read back what it stored.
            if (count != 2)
            {
                return -1;
            }
            break;

        case FastOp::kJ:
        case FastOp::kJr:
        case FastOp::kBeq:
        case FastOp::kBne:
        case FastOp::kBlez:
        case FastOp::kBgtz:
        case FastOp::kBltz:
        case FastOp::kBgez:
        case FastOp::kBeql:
        case FastOp::kBnel:
        case FastOp::kBlezl:
        case FastOp::kBgtzl:
        case FastOp::kBltzl:
        case FastOp::kBgezl:
            // The one branch is the last but one instruction.
            if (i + 2 != count)
            {
                return -1;
            }
            break;

        default:
            return -1;
        }

        if (destination >= 0 && std::find(out.begin(), out.begin() + found, destination) == out.begin() + found)
        {
            if (found == kMaxWaitRegisters)
            {
                return -1;
            }

            out[static_cast<size_t>(found++)] = static_cast<uint8_t>(destination);
        }
    }

    return found;
}

bool Cpu::RunFast(uint64_t until)
{
    const uint32_t pc32 = static_cast<uint32_t>(pc_);
    if (!KernelMode() || (status_ & kStatusKx) != 0 || branch_pending_ || (pc32 & 3) != 0 || SignExtend32(pc32) != pc_)
    {
        return false;
    }

    uint32_t page_paddr = 0;
    if (!Direct(pc_, page_paddr))
    {
        const uint32_t mapping = read_pages_.Get(pc32 >> kPageBits);
        if (mapping == 0)
        {
            return false;
        }

        page_paddr = mapping;
    }

    page_paddr &= ~kPageOffsetMask;
    const uint32_t load_limit = system_.DirectRdramSize();
    if (page_paddr >= load_limit)
    {
        return false;
    }

    // COP1's registers without traps: with FR clear, an odd single register is the high half of an even pair.
    const bool cop1 = (status_ & kStatusCu1) != 0 && !system_.GetOptions().fpu_traps;
    const bool fr = (status_ & kStatusFr) != 0;

    // Nothing that RunFast runs can change these. An event that changes them ends the run.
    const uint64_t ticks = system_.TicksPerInstruction();
    const uint64_t due = system_.NextEvent();
    const uint32_t store_limit = system_.PlainStoreLimit();
    uint8_t* const rdram = system_.RdramData();
    const uint8_t* const code = rdram + page_paddr;
    FastInstruction* const page = FastPageAt(page_paddr).data();
    const uint64_t base = pc_ & ~uint64_t{kPageOffsetMask};
    uint64_t* const gpr = gpr_.data();

    uint32_t index = (pc32 & kPageOffsetMask) >> 2;
    uint64_t now = now_;
    uint64_t steps = 0;
    bool in_slot = false; // The instruction at `index` is in a delay slot.
    bool jump = false;    // The branch before that slot was taken, to jump_target.
    uint64_t jump_target = 0;
    uint32_t loop_branch = kNoBranch;  // That branch ends a short loop that may be a wait.
    uint64_t vi_change = ~uint64_t{0}; // When the VI_CURRENT that this pass of the loop read can next change.

    struct
    {
        uint32_t branch = kNoBranch;
        int count = 0;
        int changes = 0;
        uint64_t now = 0;
        uint64_t steps = 0;
        std::array<uint8_t, kMaxWaitRegisters> registers = {};
        std::array<uint64_t, kMaxWaitRegisters> values = {};
    } watch;

    // These lambdas are always inlined. A call would keep the clock and the program counter in memory for the whole
    // loop.

    // Leaves the state for Step, as if `index` were the next instruction to run. `slot` says that it's in a delay slot.
    const auto leave = [&](bool slot) USF_INLINE_LAMBDA
    {
        now_ = now;
        instructions_ += steps;
        pc_ = base + uint64_t{index} * 4;
        next_pc_ = slot && jump ? jump_target : pc_ + 4;
        branch_pending_ = slot;
    };

    // Translates a load's or a store's address. Returns false for what has to go through Step: a misaligned address, no
    // mapping or a device.
    const auto translate = [&](uint64_t vaddr, uint32_t size, const PageTable& pages, uint32_t limit, uint32_t& paddr)
                               USF_INLINE_LAMBDA
    {
        if ((vaddr & (size - 1)) != 0)
        {
            return false;
        }

        if (!Direct(vaddr, paddr))
        {
            const uint32_t address = static_cast<uint32_t>(vaddr);
            const uint32_t mapping = pages.Get(address >> kPageBits);
            if (mapping == 0 || SignExtend32(address) != vaddr)
            {
                return false;
            }

            paddr = (mapping & ~kPageOffsetMask) | (address & kPageOffsetMask);
        }

        return paddr < limit;
    };

    for (;;)
    {
        if (now >= until || index >= kFastPageInstructions)
        {
            leave(in_slot);
            return true;
        }

        const uint32_t word = xsflib::LoadBe32(code + index * 4);
        FastInstruction& instruction = page[index];
        if (instruction.word != word)
        {
            instruction = DecodeFast(word);
        }

        const uint64_t immediate = static_cast<uint64_t>(static_cast<int64_t>(instruction.immediate));
        const uint32_t rs = instruction.rs;
        const uint32_t rt = instruction.rt;
        const uint32_t rd = instruction.rd;
        const uint64_t here = base + uint64_t{index} * 4;

        // A branch in a delay slot, and a branch that may be an idle loop, go through Step. Returns false for those.
        const auto branch = [&](bool taken, uint64_t target) USF_INLINE_LAMBDA
        {
            if (in_slot)
            {
                return false;
            }

            if (taken)
            {
                if (target == here &&
                    (index + 1 == kFastPageInstructions || xsflib::LoadBe32(code + (index + 1) * 4) == 0))
                {
                    return false;
                }

                jump = true;
                jump_target = target;
                if (!instruction.not_a_wait && target <= here && here - target <= kMaxWaitDistance * 4 &&
                    index + 1 < kFastPageInstructions && (target & ~uint64_t{kPageOffsetMask}) == base)
                {
                    loop_branch = index;
                }
            }

            return true;
        };

        bool check = in_slot;
        bool next_in_slot = false;
        switch (instruction.op)
        {
        case FastOp::kOther:
            leave(in_slot);
            return false;

        case FastOp::kNop:
            break;

        case FastOp::kSll:
            gpr[rd] = SignExtend32(static_cast<uint32_t>(gpr[rt]) << instruction.immediate);
            break;

        case FastOp::kSrl:
            gpr[rd] = SignExtend32(static_cast<uint32_t>(gpr[rt]) >> instruction.immediate);
            break;

        case FastOp::kSra:
            gpr[rd] = SignExtend32(static_cast<uint64_t>(static_cast<int64_t>(gpr[rt]) >> instruction.immediate));
            break;

        case FastOp::kSllv:
            gpr[rd] = SignExtend32(static_cast<uint32_t>(gpr[rt]) << (gpr[rs] & 31));
            break;

        case FastOp::kSrlv:
            gpr[rd] = SignExtend32(static_cast<uint32_t>(gpr[rt]) >> (gpr[rs] & 31));
            break;

        case FastOp::kSrav:
            gpr[rd] = SignExtend32(static_cast<uint64_t>(static_cast<int64_t>(gpr[rt]) >> (gpr[rs] & 31)));
            break;

        case FastOp::kJr:
            if (!branch(true, gpr[rs]))
            {
                leave(in_slot);
                return false;
            }

            next_in_slot = true;
            break;

        case FastOp::kJalr:
            {
                const uint64_t target = gpr[rs];
                if (!branch(true, target))
                {
                    leave(in_slot);
                    return false;
                }

                gpr[rd] = here + 8;
                next_in_slot = true;
                break;
            }

        case FastOp::kMfhi:
            gpr[rd] = hi_;
            break;

        case FastOp::kMflo:
            gpr[rd] = lo_;
            break;

        case FastOp::kMult:
            {
                // The hardware multiplies all 64 bits of rs by the low 35 bits of rt. Operands that aren't
                // sign-extended 32-bit values show it.
                const uint64_t t35 = static_cast<uint64_t>(static_cast<int64_t>(gpr[rt] << 29) >> 29);
                const uint64_t product = gpr[rs] * t35;
                lo_ = SignExtend32(product);
                hi_ = SignExtend32(product >> 32);
                break;
            }

        case FastOp::kMultu:
            {
                const uint64_t product =
                    static_cast<uint64_t>(static_cast<uint32_t>(gpr[rs])) * static_cast<uint32_t>(gpr[rt]);
                lo_ = SignExtend32(product);
                hi_ = SignExtend32(product >> 32);
                break;
            }

        case FastOp::kAddu:
            gpr[rd] = SignExtend32(gpr[rs] + gpr[rt]);
            break;

        case FastOp::kSubu:
            gpr[rd] = SignExtend32(gpr[rs] - gpr[rt]);
            break;

        case FastOp::kAnd:
            gpr[rd] = gpr[rs] & gpr[rt];
            break;

        case FastOp::kOr:
            gpr[rd] = gpr[rs] | gpr[rt];
            break;

        case FastOp::kXor:
            gpr[rd] = gpr[rs] ^ gpr[rt];
            break;

        case FastOp::kNor:
            gpr[rd] = ~(gpr[rs] | gpr[rt]);
            break;

        case FastOp::kSlt:
            gpr[rd] = static_cast<int64_t>(gpr[rs]) < static_cast<int64_t>(gpr[rt]) ? 1 : 0;
            break;

        case FastOp::kSltu:
            gpr[rd] = gpr[rs] < gpr[rt] ? 1 : 0;
            break;

        case FastOp::kJ:
            if (!branch(true, ((here + 4) & mips::kJumpRegionMask) | immediate))
            {
                leave(in_slot);
                return false;
            }

            next_in_slot = true;
            break;

        case FastOp::kJal:
            if (!branch(true, ((here + 4) & mips::kJumpRegionMask) | immediate))
            {
                leave(in_slot);
                return false;
            }

            gpr[mips::kRa] = here + 8;
            next_in_slot = true;
            break;

        case FastOp::kBeq:
        case FastOp::kBne:
        case FastOp::kBlez:
        case FastOp::kBgtz:
        case FastOp::kBltz:
        case FastOp::kBgez:
            {
                bool taken = false;
                switch (instruction.op)
                {
                case FastOp::kBeq:
                    taken = gpr[rs] == gpr[rt];
                    break;
                case FastOp::kBne:
                    taken = gpr[rs] != gpr[rt];
                    break;
                case FastOp::kBlez:
                    taken = static_cast<int64_t>(gpr[rs]) <= 0;
                    break;
                case FastOp::kBgtz:
                    taken = static_cast<int64_t>(gpr[rs]) > 0;
                    break;
                case FastOp::kBltz:
                    taken = static_cast<int64_t>(gpr[rs]) < 0;
                    break;
                default:
                    taken = static_cast<int64_t>(gpr[rs]) >= 0;
                    break;
                }

                if (!branch(taken, here + 4 + immediate))
                {
                    leave(in_slot);
                    return false;
                }

                next_in_slot = true;
                break;
            }

        case FastOp::kBeql:
        case FastOp::kBnel:
        case FastOp::kBlezl:
        case FastOp::kBgtzl:
        case FastOp::kBltzl:
        case FastOp::kBgezl:
            {
                bool taken = false;
                switch (instruction.op)
                {
                case FastOp::kBeql:
                    taken = gpr[rs] == gpr[rt];
                    break;
                case FastOp::kBnel:
                    taken = gpr[rs] != gpr[rt];
                    break;
                case FastOp::kBlezl:
                    taken = static_cast<int64_t>(gpr[rs]) <= 0;
                    break;
                case FastOp::kBgtzl:
                    taken = static_cast<int64_t>(gpr[rs]) > 0;
                    break;
                case FastOp::kBltzl:
                    taken = static_cast<int64_t>(gpr[rs]) < 0;
                    break;
                default:
                    taken = static_cast<int64_t>(gpr[rs]) >= 0;
                    break;
                }

                if (!branch(taken, here + 4 + immediate))
                {
                    leave(in_slot);
                    return false;
                }

                if (taken)
                {
                    next_in_slot = true;
                }
                else
                {
                    // The delay slot is skipped, and it still counts in the clock. The branch ends here.
                    index++;
                    now += ticks;
                    check = true;
                }
                break;
            }

        case FastOp::kAddiu:
            gpr[rt] = SignExtend32(gpr[rs] + immediate);
            break;

        case FastOp::kSlti:
            gpr[rt] = static_cast<int64_t>(gpr[rs]) < static_cast<int64_t>(immediate) ? 1 : 0;
            break;

        case FastOp::kSltiu:
            gpr[rt] = gpr[rs] < immediate ? 1 : 0;
            break;

        case FastOp::kAndi:
            gpr[rt] = gpr[rs] & immediate;
            break;

        case FastOp::kOri:
            gpr[rt] = gpr[rs] | immediate;
            break;

        case FastOp::kXori:
            gpr[rt] = gpr[rs] ^ immediate;
            break;

        case FastOp::kLui:
            gpr[rt] = immediate;
            break;

        case FastOp::kLb:
            {
                uint32_t paddr = 0;
                if (!translate(gpr[rs] + immediate, 1, read_pages_, load_limit, paddr))
                {
                    leave(in_slot);
                    return false;
                }

                gpr[rt] = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int8_t>(rdram[paddr])));
                break;
            }

        case FastOp::kLh:
            {
                uint32_t paddr = 0;
                if (!translate(gpr[rs] + immediate, 2, read_pages_, load_limit, paddr))
                {
                    leave(in_slot);
                    return false;
                }

                gpr[rt] =
                    static_cast<uint64_t>(static_cast<int64_t>(static_cast<int16_t>(xsflib::LoadBe16(rdram + paddr))));
                break;
            }

        case FastOp::kLw:
            {
                uint32_t paddr = 0;
                const uint64_t vaddr = gpr[rs] + immediate;
                if (!translate(vaddr, 4, read_pages_, load_limit, paddr))
                {
                    if (vaddr != kViCurrentKseg1)
                    {
                        leave(in_slot);
                        return false;
                    }

                    // A device read brings Count up to date, and this device has no side effects.
                    const uint64_t read_time = now + ticks;
                    gpr[rt] = SignExtend32(system_.LegacyViCurrent(read_time));
                    vi_change = system_.LegacyViCurrentChangeTime(read_time);
                    count_updated_ = read_time;
                    break;
                }

                gpr[rt] = SignExtend32(xsflib::LoadBe32(rdram + paddr));
                break;
            }

        case FastOp::kLbu:
            {
                uint32_t paddr = 0;
                if (!translate(gpr[rs] + immediate, 1, read_pages_, load_limit, paddr))
                {
                    leave(in_slot);
                    return false;
                }

                gpr[rt] = rdram[paddr];
                break;
            }

        case FastOp::kLhu:
            {
                uint32_t paddr = 0;
                if (!translate(gpr[rs] + immediate, 2, read_pages_, load_limit, paddr))
                {
                    leave(in_slot);
                    return false;
                }

                gpr[rt] = xsflib::LoadBe16(rdram + paddr);
                break;
            }

        case FastOp::kSb:
            {
                uint32_t paddr = 0;
                if (!translate(gpr[rs] + immediate, 1, write_pages_, store_limit, paddr))
                {
                    leave(in_slot);
                    return false;
                }

                rdram[paddr] = static_cast<uint8_t>(gpr[rt]);
                break;
            }

        case FastOp::kSh:
            {
                uint32_t paddr = 0;
                if (!translate(gpr[rs] + immediate, 2, write_pages_, store_limit, paddr))
                {
                    leave(in_slot);
                    return false;
                }

                xsflib::StoreBe16(rdram + paddr, static_cast<uint16_t>(gpr[rt]));
                break;
            }

        case FastOp::kSw:
            {
                uint32_t paddr = 0;
                if (!translate(gpr[rs] + immediate, 4, write_pages_, store_limit, paddr))
                {
                    leave(in_slot);
                    return false;
                }

                xsflib::StoreBe32(rdram + paddr, static_cast<uint32_t>(gpr[rt]));
                break;
            }

        case FastOp::kDaddu:
            gpr[rd] = gpr[rs] + gpr[rt];
            break;

        case FastOp::kDsubu:
            gpr[rd] = gpr[rs] - gpr[rt];
            break;

        case FastOp::kDaddiu:
            gpr[rt] = gpr[rs] + immediate;
            break;

        case FastOp::kDsll:
            gpr[rd] = gpr[rt] << instruction.immediate;
            break;

        case FastOp::kDsrl:
            gpr[rd] = gpr[rt] >> instruction.immediate;
            break;

        case FastOp::kDsra:
            gpr[rd] = static_cast<uint64_t>(static_cast<int64_t>(gpr[rt]) >> instruction.immediate);
            break;

        case FastOp::kLd:
            {
                uint32_t paddr = 0;
                if (!translate(gpr[rs] + immediate, 8, read_pages_, load_limit, paddr))
                {
                    leave(in_slot);
                    return false;
                }

                gpr[rt] = xsflib::LoadBe64(rdram + paddr);
                break;
            }

        case FastOp::kSd:
            {
                uint32_t paddr = 0;
                if (!translate(gpr[rs] + immediate, 8, write_pages_, store_limit, paddr))
                {
                    leave(in_slot);
                    return false;
                }

                xsflib::StoreBe64(rdram + paddr, gpr[rt]);
                break;
            }

        case FastOp::kMfc1:
            if (!cop1)
            {
                leave(in_slot);
                return false;
            }

            gpr[rt] = SignExtend32(!fr && (rd & 1) != 0 ? fpr_[rd & ~1u] >> 32 : fpr_[rd]);
            break;

        case FastOp::kMtc1:
            {
                if (!cop1)
                {
                    leave(in_slot);
                    return false;
                }

                const uint64_t value = static_cast<uint32_t>(gpr[rt]);
                if (!fr && (rd & 1) != 0)
                {
                    uint64_t& pair = fpr_[rd & ~1u];
                    pair = (pair & 0xFFFFFFFFull) | (value << 32);
                }
                else
                {
                    fpr_[rd] = (fpr_[rd] & 0xFFFFFFFF00000000ull) | value;
                }
                break;
            }

        case FastOp::kBc1:
            {
                if (!cop1)
                {
                    leave(in_slot);
                    return false;
                }

                const bool taken = ((fcr31_ & kFcr31Condition) != 0) == ((rt & 1) != 0);
                if (!branch(taken, here + 4 + immediate))
                {
                    leave(in_slot);
                    return false;
                }

                if ((rt & 2) == 0 || taken)
                {
                    next_in_slot = true;
                }
                else
                {
                    // The delay slot is skipped, and it still counts in the clock. The branch ends here.
                    index++;
                    now += ticks;
                    check = true;
                }
                break;
            }

        case FastOp::kLwc1:
            {
                uint32_t paddr = 0;
                if (!cop1 || !translate(gpr[rs] + immediate, 4, read_pages_, load_limit, paddr))
                {
                    leave(in_slot);
                    return false;
                }

                const uint64_t value = xsflib::LoadBe32(rdram + paddr);
                if (!fr && (rt & 1) != 0)
                {
                    uint64_t& pair = fpr_[rt & ~1u];
                    pair = (pair & 0xFFFFFFFFull) | (value << 32);
                }
                else
                {
                    fpr_[rt] = (fpr_[rt] & 0xFFFFFFFF00000000ull) | value;
                }
                break;
            }

        case FastOp::kLdc1:
            {
                uint32_t paddr = 0;
                if (!cop1 || !translate(gpr[rs] + immediate, 8, read_pages_, load_limit, paddr))
                {
                    leave(in_slot);
                    return false;
                }

                fpr_[fr ? rt : rt & ~1u] = xsflib::LoadBe64(rdram + paddr);
                break;
            }

        case FastOp::kSwc1:
            {
                uint32_t paddr = 0;
                if (!cop1 || !translate(gpr[rs] + immediate, 4, write_pages_, store_limit, paddr))
                {
                    leave(in_slot);
                    return false;
                }

                const uint64_t value = !fr && (rt & 1) != 0 ? fpr_[rt & ~1u] >> 32 : fpr_[rt];
                xsflib::StoreBe32(rdram + paddr, static_cast<uint32_t>(value));
                break;
            }

        case FastOp::kSdc1:
            {
                uint32_t paddr = 0;
                if (!cop1 || !translate(gpr[rs] + immediate, 8, write_pages_, store_limit, paddr))
                {
                    leave(in_slot);
                    return false;
                }

                xsflib::StoreBe64(rdram + paddr, fpr_[fr ? rt : rt & ~1u]);
                break;
            }
        }

        now += ticks;
        steps++;
        index++;
        in_slot = next_in_slot;
        if (!check)
        {
            continue;
        }

        // A check point: the end of a delay slot, or a likely branch that wasn't taken.
        uint64_t resume = base + uint64_t{index} * 4;
        bool same_page = true;
        if (jump)
        {
            jump = false;
            resume = jump_target;
            same_page = (resume & ~uint64_t{kPageOffsetMask}) == base && (resume & 3) == 0;
            index = static_cast<uint32_t>(resume & kPageOffsetMask) >> 2;
        }

        if (loop_branch != kNoBranch)
        {
            const uint32_t end = loop_branch;
            loop_branch = kNoBranch;
            if (watch.branch != end)
            {
                // The first time at the loop's end, with every instruction of the loop decoded after this. Records what
                // the loop writes.
                for (uint32_t i = index; i < end + 2; i++)
                {
                    const uint32_t body_word = xsflib::LoadBe32(code + i * 4);
                    if (page[i].word != body_word)
                    {
                        page[i] = DecodeFast(body_word);
                    }
                }

                watch.branch = end;
                watch.changes = 0;
                watch.count = WaitLoopRegisters(page + index, end + 2 - index, watch.registers);
                if (watch.count < 0)
                {
                    page[end].not_a_wait = true;
                    watch.branch = kNoBranch;
                }
            }
            else
            {
                bool same = true;
                for (int i = 0; i < watch.count; i++)
                {
                    same = same && watch.values[static_cast<size_t>(i)] == gpr[watch.registers[static_cast<size_t>(i)]];
                }
                if (same)
                {
                    // Nothing changed in a whole pass, so nothing will until an event or a new value of VI_CURRENT.
                    // Runs up to the pass at whose end the event is due, or up to the last whole pass of this run. A
                    // pass that reads VI_CURRENT has to run before its value changes.
                    watch.changes = 0;
                    const uint64_t pass_ticks = now - watch.now;
                    const uint64_t pass_steps = steps - watch.steps;
                    const uint64_t limit = std::min(due, until);
                    if (limit > now)
                    {
                        uint64_t passes = (limit - now) / pass_ticks;
                        if (due <= until && (limit - now) % pass_ticks != 0)
                        {
                            passes++;
                        }

                        passes = std::min(passes, vi_change > now ? (vi_change - now - 1) / pass_ticks : 0);
                        now += passes * pass_ticks;
                        steps += passes * pass_steps;
                    }
                }
                else if (++watch.changes == kMaxWaitChanges)
                {
                    page[end].not_a_wait = true;
                    watch.branch = kNoBranch;
                }
            }

            if (watch.branch != kNoBranch)
            {
                for (int i = 0; i < watch.count; i++)
                {
                    watch.values[static_cast<size_t>(i)] = gpr[watch.registers[static_cast<size_t>(i)]];
                }
                watch.now = now;
                watch.steps = steps;
                vi_change = ~uint64_t{0};
            }
        }
        else
        {
            // Any other check point leaves the watched loop. A pass of a later entry is no repeat of the passes before.
            watch.branch = kNoBranch;
        }

        if (now >= due || !same_page)
        {
            now_ = now;
            instructions_ += steps;
            pc_ = resume;
            next_pc_ = resume + 4;
            branch_pending_ = false;

            if (now >= due)
            {
                system_.LateCheck(now_);
            }

            count_updated_ = now_;
            return true;
        }

        count_updated_ = now;
    }
}

} // namespace usf::n64
