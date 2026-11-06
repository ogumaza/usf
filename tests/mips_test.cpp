// SPDX-License-Identifier: MIT

// The MIPS encoder against the instruction formats of the MIPS architecture's manuals, and the generic driver run on
// the core: its calls must reach the game's functions with the arguments that its options give.

#include "rip/mips.h"

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <vector>

#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

#include "n64/system.h"
#include "rip/driver.h"

namespace
{

namespace mips = usf::rip::mips;

void Encodings()
{
    // Each value is the instruction's fields packed by hand from the manuals' formats: opcode, rs, rt, immediate for
    // the I-type, and SPECIAL, rs, rt, rd, sa, function for the R-type.
    XSFLIB_CHECK(mips::Lui(mips::kT0, 0xA440) == 0x3C08A440);                // 001111 00000 01000 0xA440
    XSFLIB_CHECK(mips::Ori(mips::kA0, mips::kA0, 0x1234) == 0x34841234);     // 001101 00100 00100
    XSFLIB_CHECK(mips::Addiu(mips::kSp, mips::kSp, 0xFFE0) == 0x27BDFFE0);   // 001001 11101 11101
    XSFLIB_CHECK(mips::Andi(mips::kT5, mips::kT5, 3) == 0x31AD0003);         // 001100 01101 01101
    XSFLIB_CHECK(mips::Lw(mips::kS0, mips::kS1, 0x10) == 0x8E300010);        // 100011 10001 10000
    XSFLIB_CHECK(mips::Lbu(mips::kT8, mips::kT7, 0) == 0x91F80000);          // 100100 01111 11000
    XSFLIB_CHECK(mips::Sw(mips::kT1, mips::kT0, 4) == 0xAD090004);           // 101011 01000 01001
    XSFLIB_CHECK(mips::Sh(mips::kT1, mips::kT0, 2) == 0xA5090002);           // 101001 01000 01001
    XSFLIB_CHECK(mips::Sll(mips::kA0, mips::kS0, 2) == 0x00102080);          // rt 10000, rd 00100, sa 00010, 000000
    XSFLIB_CHECK(mips::Srl(mips::kT1, mips::kT1, 31) == 0x00094FC2);         // rt 01001, rd 01001, sa 11111, 000010
    XSFLIB_CHECK(mips::Or(mips::kT3, mips::kT1, mips::kZero) == 0x01205825); // rs 01001, rd 01011, 100101
    XSFLIB_CHECK(mips::Addu(mips::kV0, mips::kA0, mips::kA1) == 0x00851021); // rs 00100, rt 00101, rd 00010
    XSFLIB_CHECK(mips::Sltu(mips::kT2, mips::kT1, mips::kT3) == 0x012B502B); // rs 01001, rt 01011, rd 01010
    XSFLIB_CHECK(mips::Jr(mips::kRa) == 0x03E00008);
    XSFLIB_CHECK(mips::Jalr(mips::kRa, mips::kT9) == 0x0320F809);
    XSFLIB_CHECK(mips::High(0x80123456) == 0x8012 && mips::Low(0x80123456) == 0x3456);
    XSFLIB_CHECK(mips::High(0x8012F456) == 0x8013 && mips::Low(0x8012F456) == 0xF456);

    // A jump's target field is the target's word address within its 256 MiB region. A branch's offset counts words
    // from its delay slot.
    mips::Assembler code(0x80001000);
    code.J(0x80001000);
    code.Beq(mips::kT2, mips::kZero, 0x80001000);
    code.Bne(mips::kT1, mips::kT2, 0x80001010);
    XSFLIB_CHECK(code.Words()[0] == 0x08000400);
    XSFLIB_CHECK(code.Words()[1] == 0x1140FFFE); // 000100 01010 00000, -2 words from 0x80001008
    XSFLIB_CHECK(code.Words()[2] == 0x152A0001); // 000101 01001 01010, +1 word from 0x8000100C

    // LoadImmediate takes one ADDIU for a value that sign-extends from 16 bits. Otherwise LoadImmediate takes a LUI,
    // with an ORI when the low half isn't zero.
    mips::Assembler values(0x80000000);
    values.LoadImmediate(mips::kA0, 0xFFFFFFF0);
    values.LoadImmediate(mips::kA1, 0x00010000);
    values.LoadImmediate(mips::kA2, 0x80123456);
    XSFLIB_CHECK(values.Words() == (std::vector<uint32_t>{0x2404FFF0, 0x3C050001, 0x3C068012, 0x34C63456}));
}

// Physical addresses of the test's program.
constexpr uint32_t kDriver = 0x00010000;
constexpr uint32_t kStore = 0x00020000;    // A function that stores a0 to a3 at kResults.
constexpr uint32_t kCounters = 0x00020100; // Functions that each add 1 to a word at kResults + 0x10 and on.
constexpr uint32_t kResults = 0x00030000;
constexpr uint32_t kKseg0 = 0x80000000;

void WriteWords(usf::n64::System& system, uint32_t address, const std::vector<uint32_t>& words)
{
    for (size_t i = 0; i < words.size(); i++)
    {
        system.WritePhysical32(address + 4 * static_cast<uint32_t>(i), words[i]);
    }
}

// The function at kCounters + 0x20 * n adds 1 to the word at kResults + 0x10 + 4 * n.
uint32_t Counter(uint32_t n)
{
    return kKseg0 | (kCounters + 0x20 * n);
}

uint32_t CounterValue(usf::n64::System& system, uint32_t n)
{
    return system.ReadPhysical32(kResults + 0x10 + 4 * n);
}

// Runs the driver for `track` for half a second on the core with the legacy timing.
std::unique_ptr<usf::n64::System> RunDriver(const usf::rip::Driver& driver, uint32_t track)
{
    usf::n64::Options core;
    core.timing = usf::n64::Timing::kLegacy;
    auto owner = std::make_unique<usf::n64::System>(core);
    usf::n64::System& system = *owner;
    system.LoadRom(std::vector<uint8_t>(0x1000, 0));

    WriteWords(system, driver.address - kKseg0, driver.code);
    system.WritePhysical32(driver.track_word - kKseg0, track);

    WriteWords(system, kStore,
               {mips::Lui(mips::kT0, mips::High(kKseg0 | kResults)), mips::Sw(mips::kA0, mips::kT0, 0),
                mips::Sw(mips::kA1, mips::kT0, 4), mips::Sw(mips::kA2, mips::kT0, 8),
                mips::Sw(mips::kA3, mips::kT0, 12), mips::Jr(mips::kRa), mips::kNop});
    for (uint32_t n = 0; n < 4; n++)
    {
        const auto offset = static_cast<uint16_t>(0x10 + 4 * n);
        WriteWords(system, Counter(n) - kKseg0,
                   {mips::Lui(mips::kT0, mips::High(kKseg0 | kResults)), mips::Lw(mips::kT1, mips::kT0, offset),
                    mips::Addiu(mips::kT1, mips::kT1, 1), mips::Sw(mips::kT1, mips::kT0, offset), mips::Jr(mips::kRa),
                    mips::kNop});
    }
    WriteWords(system, kCounters + 0x80, {mips::Lui(mips::kV0, 0x1234), mips::Jr(mips::kRa), mips::kNop});

    usf::n64::Cpu& cpu = system.GetCpu();
    cpu.SetCop0(usf::n64::kCop0Status, 0);
    cpu.SetGpr(mips::kSp, 0xFFFFFFFF80300000ull);
    cpu.SetPc(0xFFFFFFFF00000000ull | driver.address);

    system.RunSeconds(0.5);

    return owner;
}

// The driver calls the start functions once with the track's number and the frame functions once a frame, each only
// for the tracks and frames that its conditions give.
void DriverRun()
{
    usf::rip::CallDriverOptions options;
    options.address = kKseg0 | kDriver;
    usf::rip::GameCall store;
    store.function = kKseg0 | kStore;
    store.arguments = {{0x5, true, 4}, {0x1234, false, 0}, {0, true, 0}, {0xFFFFFFFF, false, 0}};
    usf::rip::GameCall odd_start;
    odd_start.function = Counter(3);
    odd_start.track_mask = 0x1;
    odd_start.track_value = 0x1;
    options.start_calls = {store, odd_start};

    // Every frame; the first five frames; every frame for a track with the high bit of its low byte clear; frames 9 to
    // 11 for a track with that bit set.
    usf::rip::GameCall every;
    every.function = Counter(0);
    usf::rip::GameCall first_five = every;
    first_five.function = Counter(1);
    first_five.frames = 5;
    usf::rip::GameCall low = every;
    low.function = Counter(2);
    low.track_mask = 0x80;
    low.track_value = 0;
    usf::rip::GameCall window = every;
    window.function = Counter(3);
    window.track_mask = 0x80;
    window.track_value = 0x80;
    window.first_frame = 9;
    window.frames = 12;
    options.frame_calls = {every, first_five, low, window};

    const usf::rip::Driver driver = usf::rip::BuildCallDriver(options);

    XSFLIB_CHECK(driver.address == options.address);
    XSFLIB_CHECK(driver.track_word == driver.address + 4 * static_cast<uint32_t>(driver.code.size() - 1));

    const auto low_owner = RunDriver(driver, 0x21);
    const auto high_owner = RunDriver(driver, 0x80000080);
    usf::n64::System& low_track = *low_owner;
    usf::n64::System& high_track = *high_owner;

    XSFLIB_CHECK(low_track.ReadPhysical32(kResults) == (0x21u << 4 | 0x5));
    XSFLIB_CHECK(low_track.ReadPhysical32(kResults + 4) == 0x1234);
    XSFLIB_CHECK(low_track.ReadPhysical32(kResults + 8) == 0x21);
    XSFLIB_CHECK(low_track.ReadPhysical32(kResults + 12) == 0xFFFFFFFF);
    XSFLIB_CHECK(high_track.ReadPhysical32(kResults + 8) == 0x80000080);

    // Half a second holds 30 fields of NTSC video, give or take one at each end.
    const uint32_t frames = CounterValue(low_track, 0);
    XSFLIB_CHECK(frames >= 29 && frames <= 31);
    XSFLIB_CHECK(CounterValue(low_track, 1) == 5 && CounterValue(high_track, 1) == 5);
    XSFLIB_CHECK(CounterValue(low_track, 2) == frames && CounterValue(high_track, 2) == 0);
    XSFLIB_CHECK(CounterValue(low_track, 3) == 1 && CounterValue(high_track, 3) == 3);
}

// An argument can be the previous call's result, or a field of the track's number.
void DriverArguments()
{
    usf::rip::CallDriverOptions options;
    options.address = kKseg0 | kDriver;
    usf::rip::GameCall returns;
    returns.function = kKseg0 | (kCounters + 0x80);
    usf::rip::GameCall store;
    store.function = kKseg0 | kStore;
    store.arguments = {{.result = true},
                       {.track = true, .mask = 0xFF},
                       {.track = true, .right_shift = 8},
                       {.value = 1, .track = true, .shift = 4, .right_shift = 8, .mask = 0xF}};
    options.start_calls = {returns, store};

    const auto owner = RunDriver(usf::rip::BuildCallDriver(options), 0x2A7);
    usf::n64::System& system = *owner;

    XSFLIB_CHECK(system.ReadPhysical32(kResults) == 0x12340000);
    XSFLIB_CHECK(system.ReadPhysical32(kResults + 4) == 0xA7);
    XSFLIB_CHECK(system.ReadPhysical32(kResults + 8) == 0x2);
    XSFLIB_CHECK(system.ReadPhysical32(kResults + 12) == 0x21);
}

void DriverErrors()
{
    usf::rip::CallDriverOptions options;
    options.address = 0x00010000;

    bool threw = false;
    try
    {
        usf::rip::BuildCallDriver(options);
    }
    catch (const std::invalid_argument&)
    {
        threw = true;
    }

    XSFLIB_CHECK(threw);

    options.address = 0x80010000;
    usf::rip::GameCall call;
    call.function = 0x80020000;
    call.arguments.resize(5);
    options.start_calls = {call};

    threw = false;
    try
    {
        usf::rip::BuildCallDriver(options);
    }
    catch (const std::invalid_argument&)
    {
        threw = true;
    }

    XSFLIB_CHECK(threw);
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("mips",
                                    []
                                    {
                                        Encodings();
                                        DriverRun();
                                        DriverArguments();
                                        DriverErrors();
                                    });
}
