// SPDX-License-Identifier: MIT

// The emulated Nintendo 64: the CPU, the RSP, the RCP's interfaces and the PIF, on one clock. It boots a game from its
// ROM, runs it, and takes and loads Project64 1.4 save states. A ripper drives it through this class.
//
// The RDP doesn't run. The DP consumes its command lists at once and raises its interrupt on a full sync. Nothing is
// displayed. Controllers and the EEPROM are absent until a ripper connects them through the PIF (pif.h).

#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "n64/constants.h"
#include "n64/coverage.h"
#include "n64/cpu.h"
#include "n64/pif.h"
#include "n64/rsp.h"

namespace usf::n64
{

// How time passes.
enum class Timing
{
    // The hardware's rates: the CPU at 93.75 MHz with one instruction a cycle, Count at half that, the RSP at
    // 62.5 MHz, and the VI, AI, PI and SI on the hardware's clocks.
    kHardware,

    // The legacy players' Count model: each instruction adds 2 to Count, a video field and an audio buffer last the
    // lengths that those players give them, and an RSP task runs to completion when the CPU starts it.
    kLegacy,
};

// The board that the console's chips sit on.
enum class Board
{
    kConsole,

    // Seta's Aleck64 arcade board, as the E92 main board: 8 MiB of RAM at physical 0xC0000000 and the board's inputs at
    // 0xC0800000. Every other address from 0x80000000 up reads 0, and writes there are lost.
    kAleck64,

    // The Aleck64's E90 main board, which adds a video chip: its list of tiles at 0xD0000000 and its palette at
    // 0xD0010000. Its registers read 0.
    kAleck64E90,
};

// The inputs of the Aleck64's board. They're active low: a bit reads 0 while its switch is on or its button is held.
struct BoardInputs
{
    uint32_t switches = 0xFFFFFFFF; // The word at 0xC0800000: the DIP switches in the upper half, and JAMMA controls.
    uint32_t system = 0xFFFFFFFF;   // The word at 0xC0800004: coins, service, test and starts.

    // A mahjong panel. The game writes a row select in bits 8 to 15 of the word at 0xC0800008, and reads the row's keys
    // back in bits 16 to 23. `rows` holds the rows of the selects 0x01, 0x02, 0x04 and 0x08. Any other select, and the
    // word on a board with no panel, reads 0.
    bool panel = false;
    std::array<uint8_t, 4> rows = {0xFF, 0xFF, 0xFF, 0xFF};
};

// A scripted change of the board's inputs, from a video field on.
struct BoardInputEvent
{
    uint64_t field = 0;
    BoardInputs inputs;
};

struct Options
{
    Board board = Board::kConsole;

    uint32_t rdram_size = kRdramSize4;
    Timing timing = Timing::kHardware;

    // Graphics tasks (type 1 in the task header) don't run on the RSP. They complete as the legacy players complete
    // them: SP_STATUS gets halt, broke and signal 2, and the SP and DP interrupts follow. Turn this off to run any
    // code that the CPU starts on the RSP, as the hardware does.
    bool skip_graphics_tasks = true;

    // The CPU raises its timer interrupt when Count reaches Compare.
    bool timer_interrupt = true;

    // With the legacy timing: the AI's busy and full flags and the timing of its buffers. A set turns them on with
    // `_enableFIFOfull`. Without them, a buffer ends at the first event check after the write of AI_LEN, AI_LEN always
    // reads 0, and a loaded state's buffer doesn't play.
    bool ai_delay = true;

    // With the legacy timing, each event is handled at the first instruction boundary at or after its time. Turn this
    // off for the legacy players' timing of events:
    // - The CPU checks for a due event only at its check points: the end of each branch or jump, after its delay slot,
    //   ERET, an MTC0 to Status or Count, and a write of MI_INTR_MASK. Each check handles one event at most, in order
    //   of time and then of scheduling. An idle loop advances Count to the next event.
    // - An interrupt is taken only at a check point: inside the check whose handler raised it, or as an event at the
    //   front of the queue after a write that changes the interrupt lines. Taking it adds 2 to Count. ERET isn't
    //   counted.
    // - A handler counts the next event of its kind from Count at the check. The delays therefore add up.
    // - A device access sees Count with the accessing instruction counted.
    // - Each vertical interrupt is due a field after the previous one was handled. A write of V_SYNC recomputes the
    //   field's length in single precision. A scheduled interrupt keeps its time.
    // - An audio buffer's duration is computed when the game writes AI_LEN.
    bool exact_events = true;

    // The FPU raises its exceptions as the VR4300 does: an unimplemented operation for a subnormal or NaN operand or
    // result, and each IEEE exception that FCR31 enables. The legacy players compute on the host's FPU and raise none
    // of them. With this off, the FPU raises no exception, keeps subnormal results, and leaves FCR31's cause and flag
    // bits alone. A conversion to an integer of a NaN, or of a value that doesn't fit, then gives the most negative
    // integer. An operation that the hardware leaves to software and that has no IEEE result, such as a conversion of a
    // format to itself, leaves its destination unchanged.
    bool fpu_traps = true;

    // With the legacy timing: the delays of the PI's, the SI's and the DP's interrupts. A set turns them on with both
    // `_enablecompare` and `_enableFIFOfull`. Without them, those interrupts come at once.
    bool rcp_delays = true;

    // The debug port at 0x13FF0000 holds RAM, and a write of its length register sends text to the debug output.
    bool is_viewer = false;
};

// The boot chips, by the seed that their IPL3 receives.
enum class Cic
{
    kUnknown,
    k6101,
    k6102,
    k6103,
    k6105,
    k6106,
    k5101, // The Aleck64's boot chip.
};

struct BootInfo
{
    bool booted = false; // IPL3 reached the game's entry point.
    Cic cic = Cic::kUnknown;
    TvType tv = TvType::kNtsc;
    uint64_t entry_point = 0;
    std::string problem; // Why the boot failed, or what may be wrong with it.
};

// One buffer that the AI played: its samples in console order (left, right), and the rate.
struct AudioBuffer
{
    std::span<const int16_t> samples;
    uint32_t sample_rate;
};

// What a snapshot can't hold, and so what makes a moment unquiet.
struct QuietReport
{
    bool cpu_in_delay_slot = false; // The state can't say that the next instruction is in a delay slot.
    bool rsp_running = false;
    bool sp_interrupt_pending = false; // The legacy model raises an audio task's SP interrupt after the task.
    bool sp_dma_busy = false;
    bool pi_dma_busy = false;
    bool si_dma_busy = false;
    bool dp_busy = false;
    int ai_buffers = 0;             // Buffers playing or waiting in the AI.
    bool ai_buffer_waiting = false; // A second buffer waits. A legacy player starts without it.

    bool Quiet() const;
    std::string Describe() const;
};

class System
{
public:
    enum class Event : int
    {
        kVi,
        kAiBuffer,
        kPi,
        kSi,
        kSpDma,
        kCompare,
        kSpTask,
        kCountWrap, // Without exact events: Count reaches a multiple of 0x80000000. It does nothing.
        kCount,
    };

    explicit System(const Options& options = {});
    ~System();

    System(const System&) = delete;
    System& operator=(const System&) = delete;

    // Loading. LoadRom takes a ROM in any of the three byte orders and turns it into big-endian order. LoadRomImage
    // takes one that is in big-endian order already, whatever its first bytes are. Both reset the system.
    void LoadRom(std::vector<uint8_t> rom);
    void LoadRomImage(std::vector<uint8_t> rom);
    const std::vector<uint8_t>& Rom() const;

    // A dump of the PIF's boot ROM (1984 bytes), which only the CIC-NUS-6105's IPL3 needs: it reads code that the
    // PIF's boot code leaves in IMEM. Without it, IMEM stays zero and such a game may not boot.
    void SetPifRom(std::vector<uint8_t> pif_rom);

    // Resets the system and boots the cartridge: the state that the PIF's boot code leaves for the cartridge's IPL3,
    // then IPL3 itself, run as on the hardware until it jumps to the game's entry point. The boot chip is found by
    // trying each chip's seed in turn: IPL3 stops at a checksum mismatch.
    BootInfo Boot();

    // Resets the system and puts it in the state that the PIF's boot code leaves for IPL3, for the boot chip with
    // `ipl3_seed`, without running IPL3. The version bit is set only for the CIC-NUS-6101.
    void PrepareIpl3(uint8_t ipl3_seed, bool version_bit);

    // Save states.
    std::vector<uint8_t> SaveState() const;
    void LoadState(std::span<const uint8_t> state);
    QuietReport CheckQuiet() const;

    // Running. Each call runs until the clock reaches its end, until Stop is called from a callback, or until a
    // breakpoint is hit.
    void Run(uint64_t ticks);
    void RunSeconds(double seconds);
    void Stop();
    bool Stopped() const;

    // Stops the run when the CPU is about to execute the instruction at `vaddr`. Several may be set. A run that starts
    // at a breakpoint executes that instruction.
    void AddBreakpoint(uint64_t vaddr);
    void RemoveBreakpoint(uint64_t vaddr);
    void ClearBreakpoints();
    bool HitBreakpoint() const;

    // Time in ticks of the CPU clock since the last reset.
    uint64_t Now() const;

    // The video fields that have ended since the last reset.
    uint64_t Fields() const;

    uint64_t TicksPerSecond() const;
    const Options& GetOptions() const;
    TvType Tv() const;

    Cpu& GetCpu();
    const Cpu& GetCpu() const;
    Rsp& GetRsp();
    const Rsp& GetRsp() const;
    Pif& GetPif();

    // Memory, by physical address. RDRAM bytes are in console order.
    std::span<uint8_t> Rdram();
    std::span<const uint8_t> Rdram() const;
    uint32_t ReadPhysical32(uint32_t paddr);
    void WritePhysical32(uint32_t paddr, uint32_t value);

    // Memory by virtual address, translated with the CPU's current TLB and mode. Returns false where the CPU would
    // fault. These reach RDRAM and SP memory only, and they don't record coverage.
    bool ReadVirtual(uint64_t vaddr, std::span<uint8_t> bytes);
    bool WriteVirtual(uint64_t vaddr, std::span<const uint8_t> bytes);

    // Changes bytes of the ROM at a cartridge offset, growing the image if needed.
    void PatchRom(uint32_t offset, std::span<const uint8_t> bytes);

    // Coverage: every ROM word that the CPU or the PI reads, and every RDRAM word read before it is written, from
    // the moment it's turned on.
    void SetCoverage(bool enabled);
    Coverage* GetCoverage();

    // The rate that the AI's DACRATE gives now, in Hz.
    uint32_t AudioSampleRate() const;

    // Called with each buffer when the AI starts it. The span is valid during the call.
    void SetAudioCallback(std::function<void(const AudioBuffer&)> callback);

    // Called with the text that the program writes to the debug port.
    void SetDebugOutput(std::function<void(std::string_view)> callback);

    // The inputs of the Aleck64's board at reset, and a script of changes. Events take effect in the order they were
    // added, at the first read of the inputs in or after their field. Reset starts the script again.
    void SetBoardInputs(const BoardInputs& inputs);
    void AddBoardInput(const BoardInputEvent& event);

    // Loads and stores at physical addresses from 0x80000000 up since the last reset. A console has nothing there. On
    // the Aleck64, the board answers them. With the legacy timing, the count also includes the accesses through TLB
    // entries that map frames past the console's physical addresses (Cpu::RefusedBoardPages).
    uint64_t BoardAccesses() const;

    // The video frame that VI_ORIGIN points at, for tests: its origin, its width and its pixel size in bytes.
    struct Frame
    {
        uint32_t origin;
        uint32_t width;
        uint32_t bytes_per_pixel;
    };
    Frame VideoFrame() const;

    // The interfaces used by the CPU and the RSP.
    uint64_t& Clock();

    uint8_t* RdramData()
    {
        return rdram_.data();
    }

    // How much of RDRAM the CPU may read and write directly. Zero until the RDRAM chips are set up as plain memory:
    // until then, the CPU goes through ReadBus32 and WriteBus, which follow the chips' device IDs and currents.
    uint32_t DirectRdramSize() const
    {
        return direct_rdram_size_;
    }

    Coverage* ActiveCoverage()
    {
        return coverage_enabled_ ? coverage_.get() : nullptr;
    }

    uint32_t ReadBus32(uint32_t paddr);

    // The word that a load of 8 or 16 bits sees. On the PI's bus, it starts at the halfword that the load addresses.
    uint32_t ReadBusPartial32(uint32_t paddr);

    uint64_t ReadBus64(uint32_t paddr);
    void WriteBus(uint32_t paddr, uint64_t value, int size);
    void Schedule(Event event, uint64_t when);
    void Cancel(Event event);
    void RaiseMi(uint32_t bits);
    void LowerMi(uint32_t bits);
    void BreakSlice();
    bool& SliceBreakFlag();
    bool HasBreakpoints() const;
    bool ShouldBreak(uint64_t pc);
    void OnRspBroke();
    uint64_t NextEvent() const;

    // With the legacy timing: the time of the first event in the queue, an interrupt that waits included. A BREAK moves
    // Count there. All ones when nothing is queued.
    uint64_t FirstEventTime() const;

    // With the legacy timing: the time from which a check point has something to do.
    uint64_t NextCheckTime() const
    {
        return interrupt_event_ ? 0 : next_event_;
    }

    // With the legacy timing: what a read of VI_CURRENT at `time` returns, and the first later time at which the value
    // can differ. Until the next event, nothing else changes the value.
    uint32_t LegacyViCurrent(uint64_t time) const;
    uint64_t LegacyViCurrentChangeTime(uint64_t time) const;

    // Where a store may write RDRAM as plain memory, from 0 up to this address.
    uint32_t PlainStoreLimit() const
    {
        return (mi_.mode & kMiModeRepeat) != 0 ? 0 : direct_rdram_size_;
    }

    void LateCheck(uint64_t due);
    void RecheckInterrupts();
    void OnCountWritten();
    void OnSpStatusWritten(uint32_t old_status);
    uint64_t TicksPerInstruction() const;

    // DP registers, which the RSP reaches through COP0 registers 8 to 15.
    uint32_t ReadDp(uint32_t index);
    void WriteDp(uint32_t index, uint32_t value);

    // RDRAM accesses by the DMA engines, with coverage.
    void DmaReadRdram(uint32_t address, uint8_t* out, uint32_t length);
    void DmaWriteRdram(uint32_t address, const uint8_t* data, uint32_t length);

private:
    static constexpr uint64_t kNever = ~uint64_t{0};

    struct Mi
    {
        uint32_t mode = 0;
        uint32_t version = 0x02020102;
        uint32_t interrupt = 0;
        uint32_t mask = 0;
    };

    struct Vi
    {
        std::array<uint32_t, 14> registers{};
        uint64_t next_interrupt = 0; // When the next vertical interrupt comes.
        uint64_t field_ticks = 0;    // The length of a field.
        uint32_t field = 0;          // The interlaced field: 0 or 1.
        uint64_t fields = 0;         // The fields that have ended since reset.
    };

    struct Ai
    {
        uint32_t dram_address = 0;
        uint32_t length = 0;
        uint32_t control = 0;
        uint32_t dac_rate = 0;
        uint32_t bit_rate = 0;

        // The buffer that plays and the one that waits.
        struct Buffer
        {
            uint32_t address = 0;
            uint32_t length = 0;
            uint64_t start = 0;
            uint64_t duration = 0; // Without exact events, computed at the write of AI_LEN.
            bool started = false;
            bool from_state = false; // The buffer that a loaded state's AI_LEN describes.
        };
        std::array<Buffer, 2> buffers{};
        int count = 0;

        // With the legacy timing, MI_INTR's AI bit follows this latch at each re-check.
        bool interrupt_latch = false;
    };

    struct Pi
    {
        uint32_t dram_address = 0;
        uint32_t cart_address = 0;
        uint32_t read_length = 0x7F;
        uint32_t write_length = 0x7F;
        std::array<uint32_t, 8> domains{}; // LAT, PWD, PGS and RLS of domain 1, then of domain 2.
        bool busy = false;
        bool error = false;
        uint32_t latch = 0; // A word written to the cartridge's address space.
        bool latch_valid = false;
        uint64_t latch_until = 0; // With the hardware's timing, the latch empties at this time.
    };

    struct Si
    {
        uint32_t dram_address = 0;
        uint32_t pif_address_read = 0;
        uint32_t pif_address_write = 0;
        bool busy = false;
    };

    struct Dp
    {
        uint32_t start = 0;
        uint32_t end = 0;
        uint32_t current = 0;
        uint32_t status = 0x80; // The command buffer is ready.
        uint32_t clock = 0;
        uint32_t buffer_busy = 0;
        uint32_t pipe_busy = 0;
        uint32_t tmem = 0;
    };

    struct Ri
    {
        std::array<uint32_t, 8> registers{};
    };

    // The Aleck64's board.
    struct Aleck64
    {
        std::vector<uint8_t> ram;
        std::vector<uint8_t> tiles;
        std::vector<uint8_t> palette;
        BoardInputs initial_inputs;
        BoardInputs inputs;
        std::vector<BoardInputEvent> script;
        size_t next_input = 0;
        uint32_t panel_select = 0;
    };

    // One RDRAM chip's registers, which IPL3's initialization reads and writes.
    struct RdramChip
    {
        bool present = false;
        bool enable = false;
        bool auto_current = false;
        uint32_t device_id = 0; // Decoded from the DEVICE_ID register.
        uint32_t write_delay = 4;
        uint32_t current_internal = 0;
        uint32_t device_type = 0xB4190010;
        uint32_t device_id_register = 0;
        uint32_t delay = 0x23;
        uint32_t mode = 0;
        uint32_t ref_interval = 0;
        uint32_t ref_row = 0;
        uint32_t ras_interval = 0;
        uint32_t min_interval = 0;
        uint32_t address_select = 0;
        uint32_t device_manufacturer = 0x500;
        uint32_t row = 0;
        uint32_t current = 0; // The current-control value of the last MODE write.
    };

    void Reset();
    void RunLate(uint64_t end);
    void ScheduleCountWrap();
    void RecomputeNextEvent();
    void FireEvents();
    void HandleEvent(Event event);
    void UpdateCpuInterrupt();
    void CatchUpRsp();
    bool BootWithSeed(uint8_t ipl3_seed, bool version_bit, uint64_t& entry_point);

    // Buses.
    uint32_t ReadRcp32(uint32_t paddr);
    void WriteRcp32(uint32_t paddr, uint32_t value);
    uint32_t ReadCart32(uint32_t paddr);
    void WriteCart32(uint32_t paddr, uint32_t value);
    uint32_t ReadRom32(uint32_t offset);

    // Devices (rcp.cpp).
    uint32_t ReadMi(uint32_t index);
    void WriteMi(uint32_t index, uint32_t value);
    uint32_t ReadVi(uint32_t index);
    void WriteVi(uint32_t index, uint32_t value);
    uint32_t ReadAi(uint32_t index);
    void WriteAi(uint32_t index, uint32_t value);
    uint32_t ReadPi(uint32_t index);
    void WritePi(uint32_t index, uint32_t value);
    uint32_t ReadSi(uint32_t index);
    void WriteSi(uint32_t index, uint32_t value);
    uint32_t ReadRi(uint32_t index);
    void WriteRi(uint32_t index, uint32_t value);
    uint32_t ReadRdramRegister(uint32_t paddr);
    void WriteRdramRegister(uint32_t paddr, uint32_t value);
    RdramChip* SelectRdramChip(uint32_t select);
    void ResetRdramChips(bool configured);
    void UpdateRdramMapping();
    uint32_t ReadMappedRdram32(uint32_t paddr);
    void WriteMappedRdram(uint32_t paddr, uint64_t value, int size);
    void WriteRdramRepeated(uint32_t paddr, uint64_t value, int size);
    uint32_t ReadRdramChipRegister(const RdramChip& chip, uint32_t index) const;
    void WriteRdramChipRegister(RdramChip& chip, uint32_t index, uint32_t value);
    void WriteDpStatus(uint32_t value);
    void RunDpCommands();
    void WriteIsViewer(uint32_t offset, uint32_t value);

    // The Aleck64's board (aleck64.cpp).
    void ResetBoard();
    uint32_t ReadBoard32(uint32_t paddr);
    void WriteBoard(uint32_t paddr, uint64_t value, int size);
    uint32_t ReadBoardInputs(uint32_t paddr);

    // Timing of the devices, in ticks.
    uint64_t VideoClock() const;
    uint64_t FieldTicks() const;
    uint64_t ViInterruptOffset() const;
    bool AiDelay() const;
    bool RcpDelays() const;
    bool LateEvents() const;
    uint64_t LegacyFieldTicksSingle() const;
    uint64_t AiBufferTicks(uint32_t length) const;
    uint64_t PiTransferTicks(uint32_t cart_address, uint32_t length) const;
    void CopyPiToRdram(uint32_t cart, uint32_t dram, uint32_t length, uint32_t& cart_end, uint32_t& dram_end);
    void CopyRomByte(uint32_t cart, uint32_t dram);
    int64_t FieldStartAt(uint64_t time) const;
    void StartVi();
    void ScheduleVi();
    void FinishViField();
    void StartAiBuffer();
    void FinishAiBuffer();
    void FinishPiDma();
    void StartSiDma(bool to_rdram);
    void FinishSiDma();
    void StartRspTask();
    void FinishSkippedTask();

    Options options_;
    uint64_t now_ = 0;
    std::array<uint64_t, static_cast<int>(Event::kCount)> events_{};
    std::array<uint64_t, static_cast<int>(Event::kCount)> event_order_{}; // When each event was scheduled.
    uint64_t event_serial_ = 0;
    uint64_t next_event_ = kNever;
    bool interrupt_event_ = false;      // Without exact events, an interrupt waits at the front of the queue.
    uint64_t interrupt_event_time_ = 0; // When that interrupt was queued.
    bool in_event_handler_ = false;
    bool stop_ = false;
    bool slice_break_ = false;
    bool hit_breakpoint_ = false;
    bool resume_at_breakpoint_ = false;
    uint64_t break_pc_ = 0;
    std::vector<uint64_t> breakpoints_;
    TvType tv_ = TvType::kNtsc;

    std::vector<uint8_t> rdram_;
    std::vector<uint8_t> rom_;
    uint32_t rom_image_size_ = 0; // The size the CPU and the PI see: a power of two, at least 1 MiB.
    std::vector<uint8_t> pif_rom_;
    std::vector<uint8_t> is_viewer_;

    std::unique_ptr<Cpu> cpu_;
    std::unique_ptr<Rsp> rsp_;
    Pif pif_;
    Mi mi_;
    Vi vi_;
    Ai ai_;
    Pi pi_;
    Si si_;
    Dp dp_;
    Ri ri_;
    Aleck64 board_;
    uint64_t board_accesses_ = 0;
    std::array<RdramChip, 4> rdram_chips_{};
    uint32_t direct_rdram_size_ = 0;
    uint32_t rdram_noise_ = 1;
    uint64_t rsp_credit_ = 0; // Two RSP cycles for every three ticks: the remainder, in thirds of a cycle.
    uint64_t rsp_clock_ = 0;  // The tick up to which the RSP has run.

    // The task that the RSP runs, as its header gave it when the RSP started.
    uint32_t task_type_ = 0;
    uint64_t task_start_ = 0;
    bool graphics_task_pending_ = false; // A skipped graphics task waits to complete.
    bool graphics_dp_pending_ = false;   // Its DP interrupt comes when it completes.
    bool sp_interrupt_deferred_ = false; // The legacy model raises a task's SP interrupt late.

    std::unique_ptr<Coverage> coverage_;
    bool coverage_enabled_ = false;
    std::function<void(const AudioBuffer&)> audio_callback_;
    std::function<void(std::string_view)> debug_output_;
    std::vector<int16_t> audio_scratch_;
};

} // namespace usf::n64
