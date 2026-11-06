// SPDX-License-Identifier: MIT

// The VR4300, the Nintendo 64's CPU: an interpreter for MIPS III with 64-bit registers, COP0 with its TLB and
// exceptions, and the COP1 floating-point unit. Caches aren't modelled, and CACHE instructions do nothing.
//
// Each instruction takes a fixed number of ticks of the CPU clock (Timing in system.h). Count is half the tick count
// plus an offset that writes to it move.

#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace usf::n64
{

class System;

// The exception codes of the Cause register.
enum class ExceptionCode : uint32_t
{
    kInterrupt = 0,
    kTlbModification = 1,
    kTlbLoad = 2,
    kTlbStore = 3,
    kAddressErrorLoad = 4,
    kAddressErrorStore = 5,
    kBusErrorInstruction = 6,
    kBusErrorData = 7,
    kSyscall = 8,
    kBreakpoint = 9,
    kReservedInstruction = 10,
    kCoprocessorUnusable = 11,
    kOverflow = 12,
    kTrap = 13,
    kFloatingPoint = 15,
    kWatch = 23,
};

// The COP0 registers by number.
enum Cop0Register : int
{
    kCop0Index = 0,
    kCop0Random = 1,
    kCop0EntryLo0 = 2,
    kCop0EntryLo1 = 3,
    kCop0Context = 4,
    kCop0PageMask = 5,
    kCop0Wired = 6,
    kCop0BadVAddr = 8,
    kCop0Count = 9,
    kCop0EntryHi = 10,
    kCop0Compare = 11,
    kCop0Status = 12,
    kCop0Cause = 13,
    kCop0Epc = 14,
    kCop0PrId = 15,
    kCop0Config = 16,
    kCop0LlAddr = 17,
    kCop0WatchLo = 18,
    kCop0WatchHi = 19,
    kCop0XContext = 20,
    kCop0ParityError = 26,
    kCop0CacheError = 27,
    kCop0TagLo = 28,
    kCop0TagHi = 29,
    kCop0ErrorEpc = 30,
};

// Status register bits.
inline constexpr uint32_t kStatusIe = 1u << 0;
inline constexpr uint32_t kStatusExl = 1u << 1;
inline constexpr uint32_t kStatusErl = 1u << 2;
inline constexpr uint32_t kStatusUx = 1u << 5;
inline constexpr uint32_t kStatusSx = 1u << 6;
inline constexpr uint32_t kStatusKx = 1u << 7;
inline constexpr uint32_t kStatusBev = 1u << 22;
inline constexpr uint32_t kStatusFr = 1u << 26;
inline constexpr uint32_t kStatusCu0 = 1u << 28;
inline constexpr uint32_t kStatusCu1 = 1u << 29;
inline constexpr uint32_t kStatusCu2 = 1u << 30;

// Cause register bits.
inline constexpr uint32_t kCauseIp2 = 1u << 10; // The RCP's interrupt line, from the MI.
inline constexpr uint32_t kCauseIp7 = 1u << 15; // The timer interrupt.
inline constexpr uint32_t kCauseBd = 1u << 31;

// One of the 32 TLB entries, as TLBWI and TLBWR write it.
struct TlbEntry
{
    uint32_t page_mask = 0;
    uint64_t entry_hi = 0; // R, VPN2 and ASID, as in EntryHi.
    uint32_t entry_lo0 = 0;
    uint32_t entry_lo1 = 0;
    bool global = false;
};

// The CPU's registers, for a save state and for the API.
struct CpuState
{
    std::array<uint64_t, 32> gpr{};
    uint64_t hi = 0;
    uint64_t lo = 0;
    uint64_t pc = 0;
    std::array<uint64_t, 32> fpr{}; // Raw 64-bit registers. With FR clear, a double is in the even register.
    uint32_t fcr0 = 0;
    uint32_t fcr31 = 0;
    std::array<uint64_t, 32> cop0{}; // Count holds the value that Count reads.
    bool ll_bit = false;
};

class Cpu
{
public:
    // What a memory access is for. It picks the exception codes.
    enum class Access
    {
        kFetch,
        kLoad,
        kStore,
    };

    explicit Cpu(System& system);

    // Puts the CPU in its power-on state, as the hardware's cold reset leaves it.
    void Reset();

    // Runs instructions until the system's clock reaches `until`, or until the system asks the CPU to stop. Without
    // exact events, also until a check finds an event due.
    void Run(uint64_t until);

    // Registers.
    uint64_t Gpr(int index) const;
    void SetGpr(int index, uint64_t value);
    uint64_t Pc() const;

    // Moves execution to `pc`. The next instruction runs there, outside any delay slot.
    void SetPc(uint64_t pc);

    uint64_t Hi() const;
    uint64_t Lo() const;
    void SetHi(uint64_t value);
    void SetLo(uint64_t value);

    // COP0 registers as MFC0 and DMFC0 read them. SetCop0 writes Random, BadVAddr, Context, XContext, Cause and Wired
    // directly, past MTC0's masks and side effects. SetCop0 keeps only the bits that each of these registers defines,
    // and ignores a Random outside Wired to 31.
    // Every other register takes the write as MTC0 would, with its masks and side effects, but without the check points
    // of the legacy timing.
    uint64_t Cop0(int index) const;
    void SetCop0(int index, uint64_t value);

    // FPU registers as raw 64-bit values, and the control registers.
    uint64_t Fpr(int index) const;
    void SetFpr(int index, uint64_t value);
    uint32_t Fcr31() const;
    void SetFcr31(uint32_t value);

    const TlbEntry& Tlb(int index) const;

    // Writes an entry as TLBWI would. The save state's loader uses it.
    void SetTlb(int index, const TlbEntry& entry);

    CpuState State() const;
    void SetState(const CpuState& state);

    // Translates a virtual address the way the CPU would, with the current mode and ASID, without raising an
    // exception. Returns nothing when the access would fault.
    std::optional<uint32_t> TranslateQuiet(uint64_t vaddr, bool write) const;

    // Called by the system when the MI's interrupt line (Cause IP2) changes.
    void SetInterruptLine(uint32_t cause_bit, bool raised);

    // Called by the system when Count reaches Compare. Raises the timer interrupt when `raise` is set, and schedules
    // the next match.
    void CompareReached(bool raise);

    // The Count value at the system's current time.
    uint32_t Count() const;

    // How many instructions have run since reset. Random counts down with it.
    uint64_t InstructionCount() const;

    // Forgets the cached page of instructions. The system calls it when RDRAM stops or starts being plain memory.
    void InvalidateFetchCache();

    // Called with each exception that the CPU takes, before it moves to the vector: the code and the address of the
    // instruction that caused it. For debugging and for a ripper that watches for crashes.
    void SetExceptionHook(std::function<void(ExceptionCode, uint64_t)> hook);

    // With the legacy timing: Cause's IP2 comes from a state. That bit takes no interrupt until the first re-check of
    // the interrupt lines recomputes it.
    void MarkInterruptLineStale();

    // With the legacy timing: Count is up to date as of now.
    void NoteCountUpdated();

    // Without exact events (Options::exact_events): makes the CPU check for a due event after the running instruction.
    void RequestEventCheck();

    // Whether an interrupt is pending that Status allows.
    bool InterruptPending() const;

    // Without exact events: takes the pending interrupt now. EPC gets the instruction that would run next.
    void EnterInterrupt();

    // Whether the next instruction is in a branch's delay slot. A save state can't hold that.
    bool InDelaySlot() const;

    // Whether the last instruction was a jump to itself with a delay slot that does nothing: the idle loop that most
    // games run while they wait for an interrupt.
    bool InIdleLoop() const;

private:
    enum class TlbResult
    {
        kHit,
        kMiss,
        kInvalid,
        kModified,
    };

    // The legacy players translate through tables of 4 KiB pages built from the TLB entries. The tables take no account
    // of ASIDs or the global bit, and the entry written last wins where entries overlap. A value is the page's physical
    // address with bit 0 set, or 0 for a page with no mapping.
    class PageTable
    {
    public:
        uint32_t Get(uint32_t page) const
        {
            const std::unique_ptr<Leaf>& leaf = leaves_[page >> kLeafBits];
            return leaf ? (*leaf)[page & (kLeafSize - 1)] : 0;
        }

        void Set(uint32_t page, uint32_t value);
        void Clear();

    private:
        static constexpr uint32_t kLeafBits = 10;
        static constexpr uint32_t kLeafSize = 1u << kLeafBits;

        using Leaf = std::array<uint32_t, kLeafSize>;

        std::array<std::unique_ptr<Leaf>, kLeafSize> leaves_;
    };

    enum class FastOp : uint8_t
    {
        kOther, // Goes through Step.
        kNop,
        kSll,
        kSrl,
        kSra,
        kSllv,
        kSrlv,
        kSrav,
        kJr,
        kJalr,
        kMfhi,
        kMflo,
        kMult,
        kMultu,
        kAddu,
        kSubu,
        kAnd,
        kOr,
        kXor,
        kNor,
        kSlt,
        kSltu,
        kJ,
        kJal,
        kBeq,
        kBne,
        kBlez,
        kBgtz,
        kBltz,
        kBgez,
        kBeql,
        kBnel,
        kBlezl,
        kBgtzl,
        kBltzl,
        kBgezl,
        kAddiu,
        kSlti,
        kSltiu,
        kAndi,
        kOri,
        kXori,
        kLui,
        kLb,
        kLh,
        kLw,
        kLbu,
        kLhu,
        kSb,
        kSh,
        kSw,
        kDaddu,
        kDsubu,
        kDaddiu,
        kDsll,
        kDsrl,
        kDsra,
        kLd,
        kSd,
        kMfc1,
        kMtc1,
        kBc1,
        kLwc1,
        kLdc1,
        kSwc1,
        kSdc1,
    };

    // A decoded instruction. An entry counts only while `word` equals the word in RDRAM. The default entry is the
    // decoded form of a word of zeros.
    struct alignas(16) FastInstruction
    {
        uint32_t word = 0;
        FastOp op = FastOp::kNop;
        uint8_t rs = 0;
        uint8_t rt = 0;
        uint8_t rd = 0;
        int32_t immediate = 0; // Sign- or zero-extended as the instruction requires, or a shift's amount.

        // For a branch: the loop that it ends isn't a wait. WaitLoopRegisters rejected the loop, or the loop's
        // registers kept changing from pass to pass.
        bool not_a_wait = false;
    };

    using FastPage = std::array<FastInstruction, 0x400>;

    // How the current mode maps an address: kError for an address error, kTlb for a mapped address, or kDirect for an
    // unmapped segment, with the physical address in `paddr`.
    struct Segment
    {
        enum Kind
        {
            kError,
            kTlb,
            kDirect,
        };

        Kind kind;
        uint32_t paddr;
        bool extended; // The TLB refill uses the XTLB vector.
    };

    static constexpr int kMaxWaitRegisters = 6;

    // Instruction fields.
    static uint32_t Rs(uint32_t instruction);
    static uint32_t Rt(uint32_t instruction);
    static uint32_t Rd(uint32_t instruction);
    static uint32_t Sa(uint32_t instruction);
    static uint64_t SignedImmediate(uint32_t instruction);
    static uint64_t UnsignedImmediate(uint32_t instruction);

    void RunLate(uint64_t until);
    void MoveToCop0(int index, uint64_t value);
    void Step();
    void Execute(uint32_t instruction);
    void ExecuteSpecial(uint32_t instruction);
    void ExecuteRegimm(uint32_t instruction);
    void ExecuteCop0(uint32_t instruction);
    void ExecuteCop1(uint32_t instruction);
    void ExecuteLoadStore(uint32_t instruction);

    // Branches to `target` when `taken` is set. The delay slot runs either way, except after a likely branch that isn't
    // taken. An instruction that links writes the return address before it calls one of these.
    void Branch(bool taken, uint64_t target);
    void BranchLikely(bool taken, uint64_t target);
    uint64_t BranchTarget(uint32_t instruction) const;

    // Whether 64-bit operations are allowed in the current mode: kernel mode, or the mode's 64-bit addressing bit.
    bool Allows64Bit() const;

    bool RequireCop1();

    // Exceptions. Each sets up the exception entry and moves execution to the vector.
    void RaiseException(ExceptionCode code, uint32_t coprocessor = 0);
    void RaiseAddressError(uint64_t vaddr, Access access);
    void RaiseTlbException(uint64_t vaddr, Access access, bool refill, bool modification);

    // The exception of a fetch or a load with no mapping in the tables of pages. `covered` says that an entry covers
    // the address.
    void RaisePageFault(uint64_t vaddr, Access access, bool covered);

    void TakeInterrupt();
    void UpdateInterruptPending();

    // Address translation. On failure, raises the exception and returns false.
    bool Translate(uint64_t vaddr, Access access, uint32_t& paddr);

    TlbResult LookupTlb(uint64_t vaddr, bool write, uint32_t& paddr) const;

    // The legacy timing's short path (cpu_fast.cpp). Runs instructions until one of them needs Step, or until a check
    // point ends the run of a page. Returns false if Step must run the next instruction.
    bool RunFast(uint64_t until);

    static FastInstruction DecodeFast(uint32_t instruction);
    FastPage& FastPageAt(uint32_t paddr);

    // The registers that a loop of `count` instructions writes, its branch and delay slot last. Returns their number,
    // or -1 if the loop holds anything that keeps it from being a wait.
    static int WaitLoopRegisters(const FastInstruction* first, uint32_t count,
                                 std::array<uint8_t, kMaxWaitRegisters>& out);

    TlbResult LookupTlbPages(uint64_t vaddr, bool write, uint32_t& paddr) const;
    void UnmapTlbPages(const TlbEntry& entry);
    void MapTlbPages(const TlbEntry& entry);
    void SetAddressFields(uint64_t vaddr);

    Segment Decode(uint64_t vaddr) const;

    // Memory accesses through translation.
    bool Fetch(uint32_t& instruction);
    template <typename T>
    bool Load(uint64_t vaddr, T& value);
    template <typename T>
    bool Store(uint64_t vaddr, uint64_t value);

    // The physical half of a load, after translation.
    template <typename T>
    void LoadPhysical(uint32_t paddr, T& value);

    // COP0.
    uint64_t ReadCop0(int index) const;
    void WriteCop0(int index, uint64_t value);
    void WriteStatus(uint32_t value);
    void WriteCount(uint32_t value);
    void WriteCompare(uint32_t value);
    uint32_t Random() const;
    void TlbRead();
    void TlbWrite(int index);
    void TlbProbe();
    void Eret();
    void ScheduleCompare();

    // COP1 (cpu_fpu.cpp). With FR clear, the moves and the loads and stores see 16 registers of 64 bits: an odd
    // register is the high half of the even one. The operations read fs from the even register of the pair, and ft
    // and fd from the register named.
    template <typename T>
    void ExecuteFpuFormat(uint32_t instruction);
    void ExecuteFpuInteger(uint32_t instruction, bool doubleword);
    uint32_t MoveFromFpr32(int index) const;
    void MoveToFpr32(int index, uint32_t value);
    uint64_t MoveFromFpr64(int index) const;
    void MoveToFpr64(int index, uint64_t value);
    uint32_t SourceSingle(int index) const;
    uint64_t SourceDouble(int index) const;
    void WriteFcr31(uint32_t value);

    // The FPU's exception checks. Each sets the cause bits, and the flag bits of the exceptions that don't trap. Each
    // returns false when it has raised an exception, and the operation must then leave its result unwritten.
    bool FpuStart();
    bool FpuUnimplemented();
    bool FpuSignal(uint32_t exceptions);
    bool FpuSubnormalTraps() const;
    bool FpuTraps() const;
    int FpuRounding() const;
    bool FpuCheckHostFlags(int host_flags, bool conversion);
    template <typename T>
    bool FpuCheckInput(T value);
    template <typename T>
    bool FpuCheckInputs(T a, T b);
    template <typename T>
    bool FpuCheckOutput(T& value);
    template <typename T>
    void FpuCompare(uint32_t condition, T a, T b);
    template <typename T>
    void FpuConvertToInteger(uint32_t instruction, T value, bool doubleword, int rounding);

    System& system_;
    uint64_t& now_;

    std::array<uint64_t, 32> gpr_{};
    uint64_t hi_ = 0;
    uint64_t lo_ = 0;

    // The instruction now running, the next one, and the one after it. A taken branch sets the third.
    uint64_t current_pc_ = 0;
    uint64_t pc_ = 0;
    uint64_t next_pc_ = 0;
    bool branch_pending_ = false; // The instruction at pc_ is in a delay slot.
    bool in_delay_slot_ = false;  // The running instruction is in a delay slot.
    bool idle_loop_ = false;
    bool late_events_ = false;  // The last run used the legacy timing of events.
    bool event_check_ = false;  // Without exact events, check for a due event after the running instruction.
    bool status_check_ = false; // The same after an MTC0 to Status. The test for a due event leaves the MTC0 out.

    // COP0.
    uint32_t index_ = 0;
    uint32_t entry_lo0_ = 0;
    uint32_t entry_lo1_ = 0;
    uint64_t context_ = 0;
    uint32_t page_mask_ = 0;
    uint32_t wired_ = 0;
    uint64_t bad_vaddr_ = 0;
    uint64_t count_offset_ = 0; // Count is (now + count_offset_) / 2.
    uint64_t entry_hi_ = 0;
    uint32_t compare_ = 0;
    uint32_t status_ = 0;
    uint32_t cause_ = 0;
    uint64_t epc_ = 0;
    uint32_t config_ = 0;
    uint32_t ll_addr_ = 0;
    uint32_t watch_lo_ = 0;
    uint32_t watch_hi_ = 0;
    uint64_t x_context_ = 0;
    uint32_t parity_error_ = 0;
    uint32_t tag_lo_ = 0;
    uint64_t error_epc_ = 0;
    uint64_t latch_ = 0;        // The last value written to any COP0 register. The unused registers read it.
    uint64_t random_epoch_ = 0; // The instruction count at which Random last held 31.
    bool ll_bit_ = false;
    std::array<TlbEntry, 32> tlb_{};
    bool tlb_pages_ = false; // Translate through the tables, as the legacy players do.
    PageTable read_pages_;
    PageTable write_pages_;

    // COP1.
    std::array<uint64_t, 32> fpr_{};
    uint32_t fcr31_ = 0;
    int legacy_rounding_ = 0; // Without FPU traps, the mode that the game last wrote to FCR31, or to nearest.

    uint64_t instructions_ = 0;
    bool interrupt_pending_ = false;
    bool ip2_stale_ = false;     // Cause's IP2 is the one that a state holds, and it takes no interrupt.
    uint64_t count_updated_ = 0; // With the legacy timing: when Count was last brought up to date.
    std::function<void(ExceptionCode, uint64_t)> exception_hook_;
    uint64_t cop2_latch_ = 0;

    // A cache of the page that instructions come from, while it's in RDRAM. translation_epoch_ moves on whenever a
    // change could alter the translation.
    uint64_t fetch_page_ = ~uint64_t{0};
    uint32_t fetch_page_paddr_ = 0;
    uint64_t fetch_epoch_ = 0;
    uint64_t translation_epoch_ = 1;

    // The decoded instructions of the pages of RDRAM that RunFast has run, by physical page.
    std::vector<std::unique_ptr<FastPage>> fast_pages_;
};

} // namespace usf::n64
