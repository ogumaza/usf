// SPDX-License-Identifier: MIT

// The RSP: its scalar unit, its vector unit (rsp_vector.h), DMEM and IMEM, its DMA engine and its registers.

#pragma once

#include <array>
#include <cstdint>

#include "n64/rsp_vector.h"

namespace usf::n64
{

class System;

// The SP registers, as the save state holds them.
struct SpRegisters
{
    uint32_t mem_address = 0;
    uint32_t dram_address = 0;
    uint32_t read_length = 0;
    uint32_t write_length = 0;
    uint32_t status = 1;
    uint32_t dma_full = 0;
    uint32_t dma_busy = 0;
    uint32_t semaphore = 0;
    uint32_t pc = 0;
    uint32_t ibist = 0;
};

class Rsp
{
public:
    explicit Rsp(System& system);

    void Reset();

    // Runs up to `cycles` instructions, one a cycle. Stops early when the RSP halts. Returns the cycles run.
    uint64_t Run(uint64_t cycles);
    bool Halted() const;

    std::array<uint8_t, 0x1000>& Dmem();
    std::array<uint8_t, 0x1000>& Imem();
    const std::array<uint8_t, 0x1000>& Dmem() const;
    const std::array<uint8_t, 0x1000>& Imem() const;

    // The registers at 0x04040000 by word index, as the CPU reads and writes them.
    uint32_t ReadRegister(uint32_t index);
    void WriteRegister(uint32_t index, uint32_t value);

    // SP_PC_REG and SP_IBIST_REG at 0x04080000.
    uint32_t ReadPc() const;
    void WritePc(uint32_t value);
    uint32_t ReadIbist() const;
    void WriteIbist(uint32_t value);

    uint32_t Status() const;

    // Sets SP_STATUS bits directly, as a task that the core completes without running sets them.
    void SetStatusBits(uint32_t bits);

    // The DMA engine. The system calls FinishDma when a transfer's time is up.
    bool DmaBusy() const;
    bool DmaFull() const;
    void FinishDma();

    // Save states.
    SpRegisters Registers() const;
    void SetRegisters(const SpRegisters& registers);

    VectorUnit& Vector();
    uint32_t Gpr(int index) const;

private:
    // The operations of the scalar unit and the COP2 moves. Each form that runs differently has its value.
    enum class Op : uint8_t
    {
        kSll,
        kSrl,
        kSra,
        kSllv,
        kSrlv,
        kSrav,
        kJr,
        kJalr,
        kBreak,
        kAdd,
        kSub,
        kAnd,
        kOr,
        kXor,
        kNor,
        kSlt,
        kSltu,
        kBltz,
        kBgez,
        kBltzal,
        kBgezal,
        kJ,
        kJal,
        kBeq,
        kBne,
        kBlez,
        kBgtz,
        kAddi,
        kSlti,
        kSltiu,
        kAndi,
        kOri,
        kXori,
        kLui,
        kCop0,
        kMfc2,
        kCfc2,
        kMtc2,
        kCtc2,
        kVector,
        kLb,
        kLh,
        kLw,
        kLbu,
        kLhu,
        kSb,
        kSh,
        kSw,
        kLoadVector,
        kStoreVector,
        kNone, // The RSP has no exceptions. Other opcodes do nothing.
    };

    // An instruction with its fields taken apart. The table holds one for each word of IMEM. An entry counts only while
    // `word` equals the word in IMEM, so no write to IMEM has to tell the table. The default entry is a NOP, the
    // decoded form of a word of zeros.
    struct Decoded
    {
        uint32_t word = 0;
        Op op = Op::kSll;
        uint8_t rs = 0;
        uint8_t rt = 0;
        uint8_t rd = 0;
        uint8_t sa = 0;

        // Sign- or zero-extended as the instruction needs. A branch's holds its offset in bytes.
        uint32_t immediate = 0;

        VectorUnit::Operation vector = nullptr;
    };

    struct Dma
    {
        uint32_t mem_address = 0; // Bit 12 picks IMEM.
        uint32_t dram_address = 0;
        uint32_t length = 0; // Bytes minus one, in units of 8 (bits 3 to 11).
        uint32_t count = 0;
        uint32_t skip = 0;
        bool to_rdram = false;
    };

    static Decoded Decode(uint32_t instruction);

    void Step();
    void Execute(const Decoded& instruction);
    void ExecuteCop0(uint32_t instruction);
    void LoadVector(uint32_t instruction);
    void StoreVector(uint32_t instruction);
    void Branch(bool taken, uint32_t target);
    uint32_t ReadCop0(uint32_t index);
    void WriteCop0(uint32_t index, uint32_t value);
    void WriteStatus(uint32_t value);
    void QueueDma(uint32_t length_register, bool to_rdram);
    void StartDma();
    uint64_t DmaTicks(const Dma& dma) const;

    uint8_t Read8(uint32_t address) const;
    void Write8(uint32_t address, uint8_t value);
    uint32_t Read32(uint32_t address) const;
    void Write32(uint32_t address, uint32_t value);

    // Moves bytes `first` up to `end` of a register from or to DMEM at `address`, which wraps. The register's index
    // wraps too.
    void LoadBytes(int vt, int first, int end, uint32_t address);
    void StoreBytes(int vt, int first, int end, uint32_t address);

    System& system_;
    std::array<uint8_t, 0x1000> dmem_{};
    std::array<uint8_t, 0x1000> imem_{};
    std::array<Decoded, 0x400> decoded_{};
    std::array<uint32_t, 32> gpr_{};
    uint32_t pc_ = 0;
    uint32_t next_pc_ = 4;
    uint32_t status_ = 1;
    uint32_t semaphore_ = 0;
    uint32_t ibist_ = 0;

    // The transfer that runs and the one that waits for it.
    Dma current_;
    Dma pending_;
    bool busy_ = false;
    bool full_ = false;

    VectorUnit vu_;
};

} // namespace usf::n64
