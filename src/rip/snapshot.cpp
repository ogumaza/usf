// SPDX-License-Identifier: MIT

#include "rip/snapshot.h"

#include <array>
#include <cstdio>
#include <stdexcept>

#include "xsflib/bytes.h"
#include "xsflib/sha1.h"

namespace usf::rip
{

namespace
{

// The state image's layout.
constexpr uint32_t kStateGpr = 0x050;
constexpr uint32_t kStateRdram = 0x75C;

// libultra's OSThread: the link in the queue the thread waits in, its priority, the queue, and its state. A queue
// ends at a thread of priority -1.
constexpr uint32_t kThreadNext = 0x00;
constexpr uint32_t kThreadPriority = 0x04;
constexpr uint32_t kThreadQueue = 0x08;
constexpr uint32_t kThreadState = 0x10;
constexpr uint16_t kThreadStopped = 1;
constexpr uint16_t kThreadRunning = 4;
constexpr int kMaxQueueLength = 4096;

uint64_t SignExtend32(uint32_t value)
{
    return static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(value)));
}

std::string Hex(uint32_t value)
{
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08X", value);

    return text;
}

uint32_t ReadWord(n64::System& system, uint32_t vaddr)
{
    std::array<uint8_t, 4> bytes{};
    if (!system.ReadVirtual(SignExtend32(vaddr), bytes))
    {
        throw std::runtime_error("the CPU can't read " + Hex(vaddr));
    }

    return xsflib::LoadBe32(bytes.data());
}

void WriteBytes(n64::System& system, uint32_t vaddr, std::span<const uint8_t> bytes)
{
    if (!system.WriteVirtual(SignExtend32(vaddr), bytes))
    {
        throw std::runtime_error("the CPU can't write " + Hex(vaddr));
    }
}

void WriteWords(n64::System& system, uint32_t vaddr, const std::vector<uint32_t>& words)
{
    std::vector<uint8_t> bytes(4 * words.size());
    for (size_t i = 0; i < words.size(); i++)
    {
        xsflib::StoreBe32(bytes.data() + 4 * i, words[i]);
    }

    WriteBytes(system, vaddr, bytes);
}

void WriteWord(n64::System& system, uint32_t vaddr, uint32_t value)
{
    WriteWords(system, vaddr, {value});
}

// Connects the controllers and the EEPROM that the profile asks for, with its input script.
void ConnectInput(n64::System& system, const GameProfile& profile)
{
    n64::Pif& pif = system.GetPif();
    if (profile.controller)
    {
        pif.ConnectController(0, true);
    }

    for (const n64::InputEvent& event : profile.input)
    {
        pif.ConnectController(event.port, true);
        pif.AddInput(event);
    }

    pif.SetEeprom(profile.eeprom);
}

// Runs to the profile's snapshot point and returns at a quiet arrival there.
void RunToSnapshot(n64::System& system, const GameProfile& profile, Snapshot& snapshot)
{
    const uint64_t deadline =
        system.Now() + static_cast<uint64_t>(profile.boot_seconds * static_cast<double>(system.TicksPerSecond()));
    system.AddBreakpoint(SignExtend32(profile.snapshot_address));

    int tries = 0;
    while (true)
    {
        if (system.Now() < deadline)
        {
            system.Run(deadline - system.Now());
        }
        if (!system.HitBreakpoint())
        {
            // The message gives the boot time. For a point that the game reaches more than once, it names the arrival.
            // Once the profile's arrival has come, the point was reached but no arrival there was quiet.
            char detail[128];
            std::string message;
            if (snapshot.arrival >= profile.snapshot_hit)
            {
                if (snapshot.arrival == profile.snapshot_hit)
                {
                    std::snprintf(detail, sizeof(detail), " wasn't quiet at arrival %d", snapshot.arrival);
                }
                else
                {
                    std::snprintf(detail, sizeof(detail), " wasn't quiet at arrivals %d to %d", profile.snapshot_hit,
                                  snapshot.arrival);
                }
                message = "the snapshot point " + Hex(profile.snapshot_address) + detail;
                std::snprintf(detail, sizeof(detail), ", and the game didn't reach it again within %g s",
                              profile.boot_seconds);
                message += detail;
            }
            else
            {
                std::snprintf(detail, sizeof(detail), " within %g s", profile.boot_seconds);
                message = "the game didn't reach the snapshot point " + Hex(profile.snapshot_address) + detail;
                if (profile.snapshot_hit > 1)
                {
                    std::snprintf(detail, sizeof(detail), " (arrival %d of %d)", snapshot.arrival + 1,
                                  profile.snapshot_hit);
                    message += detail;
                }
            }

            throw std::runtime_error(message);
        }

        snapshot.arrival++;
        if (snapshot.arrival < profile.snapshot_hit)
        {
            continue;
        }

        const n64::QuietReport quiet = system.CheckQuiet();
        if (quiet.Quiet())
        {
            break;
        }
        if (tries++ >= profile.quiet_tries)
        {
            throw std::runtime_error("the snapshot point isn't quiet: " + quiet.Describe());
        }
    }

    system.ClearBreakpoints();
}

// Where each mini writes its number, as the selector or the driver gives it.
void LocateSelector(n64::System& system, const GameProfile& profile, Snapshot& snapshot)
{
    TrackSelector selector = profile.selector;
    if (profile.driver)
    {
        selector.kind = TrackSelector::Kind::kRdram;
        selector.address = profile.driver->track_word;
        selector.size = 4;
    }

    snapshot.selector_kind = selector.kind;
    if (selector.kind == TrackSelector::Kind::kRegister)
    {
        if (selector.reg < 1 || selector.reg > 31)
        {
            throw std::runtime_error("the selector's register must be r1 to r31");
        }

        snapshot.selector_offset = kStateGpr + 8 * static_cast<uint32_t>(selector.reg);
        snapshot.selector_size = 8;
        return;
    }

    if ((selector.size != 1 && selector.size != 2 && selector.size != 4) ||
        selector.address % static_cast<uint32_t>(selector.size) != 0)
    {
        throw std::runtime_error("the selector's value must be 1, 2 or 4 bytes, aligned to its size");
    }

    const auto paddr = system.GetCpu().TranslateQuiet(SignExtend32(selector.address), true);
    if (!paddr || *paddr >= system.Rdram().size())
    {
        throw std::runtime_error("the selector " + Hex(selector.address) + " isn't in RDRAM");
    }

    snapshot.selector_address = *paddr;
    snapshot.selector_value_size = selector.size;
    snapshot.selector_offset = kStateRdram + (*paddr & ~3u);
    snapshot.selector_size = 4;
}

} // namespace

Snapshot TakeSnapshot(const GameProfile& profile, std::vector<uint8_t> rom, std::span<const uint8_t> pif_rom,
                      n64::Timing timing)
{
    n64::Options options;
    options.board = profile.board;
    options.rdram_size = profile.rdram_size;
    options.timing = timing;
    n64::System system(options);
    system.SetBoardInputs(profile.board_inputs);
    for (const n64::BoardInputEvent& event : profile.board_input)
    {
        system.AddBoardInput(event);
    }

    system.LoadRom(std::move(rom));
    if (!profile.sha1.empty())
    {
        const std::string sha1 = xsflib::Sha1::Hex(xsflib::Sha1::Of(system.Rom()));
        if (sha1 != profile.sha1)
        {
            throw std::runtime_error("the image's SHA-1 is " + sha1 + ", not the profile's " + profile.sha1);
        }
    }
    for (const RomPatch& patch : profile.rom_patches)
    {
        system.PatchRom(patch.offset, patch.bytes);
    }
    if (!pif_rom.empty())
    {
        system.SetPifRom(std::vector<uint8_t>(pif_rom.begin(), pif_rom.end()));
    }
    ConnectInput(system, profile);

    Snapshot snapshot;
    const n64::BootInfo boot = system.Boot();
    if (!boot.booted)
    {
        throw std::runtime_error("the boot failed: " + boot.problem);
    }
    if (profile.cic != n64::Cic::kUnknown && boot.cic != profile.cic)
    {
        throw std::runtime_error("the boot found another boot chip than the profile's");
    }

    RunToSnapshot(system, profile, snapshot);
    snapshot.seconds = static_cast<double>(system.Now()) / static_cast<double>(system.TicksPerSecond());

    // The profile's changes, then the driver, which takes over the thread that reached the snapshot point.
    for (const RdramPatch& patch : profile.rdram_patches)
    {
        WriteWords(system, patch.address, patch.words);
    }
    for (uint32_t thread : profile.stop_threads)
    {
        StopThread(system, thread);
    }
    if (profile.driver)
    {
        WriteWords(system, profile.driver->address, profile.driver->code);
        system.GetCpu().SetPc(SignExtend32(profile.driver->address));
    }

    LocateSelector(system, profile, snapshot);
    snapshot.rom = system.Rom();
    snapshot.state = system.SaveState();

    return snapshot;
}

std::vector<uint8_t> TrackState(const Snapshot& snapshot, uint32_t number)
{
    std::vector<uint8_t> state = snapshot.state;
    if (snapshot.selector_kind == TrackSelector::Kind::kRegister)
    {
        xsflib::StoreLe64(state.data() + snapshot.selector_offset, SignExtend32(number));
        return state;
    }

    // RDRAM is in LE word order: console byte A is at A XOR 3.
    const int size = snapshot.selector_value_size;
    for (int i = 0; i < size; i++)
    {
        const uint32_t address = snapshot.selector_address + static_cast<uint32_t>(i);
        state[kStateRdram + (address ^ 3)] = static_cast<uint8_t>(number >> (8 * (size - 1 - i)));
    }

    return state;
}

std::vector<uint8_t> TrackState(const Snapshot& snapshot, const Track& track)
{
    std::vector<uint8_t> state = TrackState(snapshot, track.number);
    for (const RdramPatch& patch : track.rdram_patches)
    {
        const uint32_t offset = RdramStateOffset(patch.address);
        if (offset + 4 * patch.words.size() > state.size())
        {
            throw std::runtime_error("the patch at " + Hex(patch.address) + " runs past RDRAM");
        }

        for (size_t i = 0; i < patch.words.size(); i++)
        {
            xsflib::StoreLe32(state.data() + offset + 4 * i, patch.words[i]);
        }
    }

    return state;
}

uint32_t RdramStateOffset(uint32_t address)
{
    const uint32_t segment = address >> 29;
    if (address % 4 != 0 || (segment != 4 && segment != 5))
    {
        throw std::runtime_error("the patch's address " + Hex(address) + " must be word-aligned, in KSEG0 or KSEG1");
    }

    return kStateRdram + (address & 0x1FFFFFFF);
}

void StopThread(n64::System& system, uint32_t thread)
{
    std::array<uint8_t, 2> state_bytes{};
    if (!system.ReadVirtual(SignExtend32(thread + kThreadState), state_bytes))
    {
        throw std::runtime_error("the CPU can't read the thread at " + Hex(thread));
    }

    const uint16_t state = xsflib::LoadBe16(state_bytes.data());
    if (state == kThreadRunning)
    {
        throw std::runtime_error("the thread at " + Hex(thread) + " is running at the snapshot");
    }
    if (state == kThreadStopped)
    {
        return;
    }

    // Unlink the thread from its queue: find the link that points at it and point it at the thread's successor.
    const uint32_t queue = ReadWord(system, thread + kThreadQueue);
    if (queue != 0)
    {
        uint32_t link = queue;
        for (int i = 0; i < kMaxQueueLength; i++)
        {
            const uint32_t current = ReadWord(system, link);
            if (current == 0 || static_cast<int32_t>(ReadWord(system, current + kThreadPriority)) == -1)
            {
                break;
            }
            if (current == thread)
            {
                WriteWord(system, link, ReadWord(system, thread + kThreadNext));
                break;
            }

            link = current + kThreadNext;
        }

        WriteWord(system, thread + kThreadQueue, 0);
    }

    std::array<uint8_t, 2> stopped{};
    xsflib::StoreBe16(stopped.data(), kThreadStopped);
    WriteBytes(system, thread + kThreadState, stopped);
}

} // namespace usf::rip
