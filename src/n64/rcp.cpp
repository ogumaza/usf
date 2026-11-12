// SPDX-License-Identifier: MIT

// The RCP's interfaces: MI, VI, AI, PI, SI, RI and the DP's command registers.

#include <algorithm>

#include "xsflib/bytes.h"

#include "n64/bits.h"
#include "n64/system.h"

namespace usf::n64
{
namespace
{

// VI_STATUS bit 6: serrate, set in the interlaced modes.
constexpr uint32_t kViSerrate = 1u << 6;

// What VI_V_SYNC and VI_H_SYNC hold in each television system when the program hasn't set them.
constexpr uint32_t kDefaultVSyncNtsc = 524;
constexpr uint32_t kDefaultVSyncPal = 624;
constexpr uint32_t kDefaultHSyncNtsc = 3093;
constexpr uint32_t kDefaultHSyncPal = 3177;

// The legacy players' VI_CURRENT moves on one half-line each 1,500 count units.
constexpr uint64_t kLegacyTicksPerViLine = 2 * 1500;

// An SI transfer and the PIF's work on it take about 64 microseconds.
constexpr uint64_t kSiTransferTicks = 6000;

// The legacy players complete a PI transfer that touches no ROM 0x1000 count units after it starts.
constexpr uint64_t kLegacyEmptyPiTicks = 0x2000;

// The RDP's full sync command. It raises the DP's interrupt.
constexpr uint32_t kDpFullSync = 0x29;

// AI_STATUS bits. A full queue sets bits 31 and 0.
constexpr uint32_t kAiStatusFull = 0x80000001;
constexpr uint32_t kAiStatusBusy = 1u << 30;
constexpr uint32_t kAiStatusEnabled = 1u << 25;

// PI_STATUS bits.
constexpr uint32_t kPiStatusDmaBusy = 1u << 0;
constexpr uint32_t kPiStatusIoBusy = 1u << 1;
constexpr uint32_t kPiStatusError = 1u << 2;
constexpr uint32_t kPiStatusInterrupt = 1u << 3;

// SI_STATUS bits.
constexpr uint32_t kSiStatusDmaBusy = 1u << 0;
constexpr uint32_t kSiStatusInterrupt = 1u << 12;

// The bits of a write to DPC_STATUS that clear the TMEM, pipe, command and clock counters.
constexpr uint32_t kDpWriteClearTmem = 1u << 6;
constexpr uint32_t kDpWriteClearPipe = 1u << 7;
constexpr uint32_t kDpWriteClearCommand = 1u << 8;
constexpr uint32_t kDpWriteClearClock = 1u << 9;

// A write to a register that has a clear bit and a set bit for `flag`. The clear comes first. A write of both bits sets
// the flag.
void ApplyClearSet(uint32_t& reg, uint32_t value, int clear_bit, int set_bit, uint32_t flag)
{
    if ((value & (1u << clear_bit)) != 0)
    {
        reg &= ~flag;
    }
    if ((value & (1u << set_bit)) != 0)
    {
        reg |= flag;
    }
}

// The lengths of the RDP's commands in bytes. Triangles and texture rectangles are longer than the usual 8 bytes.
uint32_t DpCommandLength(uint32_t op)
{
    switch (op)
    {
    case 0x08:
        return 32;
    case 0x09:
        return 48;
    case 0x0A:
        return 96;
    case 0x0B:
        return 112;
    case 0x0C:
        return 96;
    case 0x0D:
        return 112;
    case 0x0E:
        return 160;
    case 0x0F:
        return 176;
    case 0x24:
    case 0x25:
        return 16;
    default:
        return 8;
    }
}

} // namespace

// MI.

uint32_t System::ReadMi(uint32_t index)
{
    switch (index)
    {
    case Mi::kMode:
        return mi_.mode;
    case 1:
        return mi_.version;
    case 2:
        return mi_.interrupt;
    default:
        return mi_.mask;
    }
}

void System::WriteMi(uint32_t index, uint32_t value)
{
    if (index == Mi::kMode)
    {
        // MI_MODE: the repeat length, and clear and set bits for init mode, ebus test mode and RDRAM register mode.
        uint32_t mode = (mi_.mode & ~kMiModeRepeatLength) | (value & kMiModeRepeatLength);
        ApplyClearSet(mode, value, 7, 8, kMiModeRepeat);
        ApplyClearSet(mode, value, 9, 10, kMiModeEbusTest);
        ApplyClearSet(mode, value, 12, 13, kMiModeRdramRegister);
        mi_.mode = mode;

        if ((value & kMiModeClearDp) != 0)
        {
            LowerMi(kMiInterruptDp);
        }
        return;
    }

    if (index == Mi::kMask)
    {
        // MI_MASK: a clear bit and a set bit for each interrupt.
        for (int i = 0; i < kMiInterruptCount; i++)
        {
            ApplyClearSet(mi_.mask, value, 2 * i, 2 * i + 1, 1u << i);
        }

        UpdateCpuInterrupt();

        // In the legacy timing, this write re-checks the interrupt lines and then checks for a due event.
        RecheckInterrupts();
        cpu_->RequestEventCheck();
    }
}

// VI.

uint64_t System::VideoClock() const
{
    switch (tv_)
    {
    case TvType::kPal:
        return kVideoClockPal;
    case TvType::kMpal:
        return kVideoClockMpal;
    default:
        return kVideoClockNtsc;
    }
}

uint64_t System::FieldRate() const
{
    return tv_ == TvType::kPal ? 50 : 60;
}

uint64_t System::VSyncOrDefault() const
{
    const uint64_t v_sync = vi_.registers[Vi::kVSync] & Vi::kLineMask;
    if (v_sync != 0)
    {
        return v_sync;
    }

    return tv_ == TvType::kPal ? kDefaultVSyncPal : kDefaultVSyncNtsc;
}

uint64_t System::ElapsedInField(uint64_t time) const
{
    return static_cast<uint64_t>(std::max<int64_t>(static_cast<int64_t>(time) - FieldStartAt(time), 0));
}

uint64_t System::FieldTicks() const
{
    if (options_.timing == Timing::kLegacy)
    {
        // (V_SYNC + 1) × L count units, where L is the video clock over the field rate over (V_SYNC + 1), rounded
        // down. A count unit is two ticks.
        const uint64_t lines = LegacyLines();
        return 2 * lines * (VideoClock() / FieldRate() / lines);
    }

    // V_SYNC + 1 half-lines, each half of (H_SYNC + 1) video clocks.
    const uint64_t v_sync = VSyncOrDefault();
    uint64_t h_sync = vi_.registers[Vi::kHSync] & Vi::kHSyncMask;
    if (h_sync == 0)
    {
        h_sync = tv_ == TvType::kPal ? kDefaultHSyncPal : kDefaultHSyncNtsc;
    }

    const uint64_t field_clocks_twice = (v_sync + 1) * (h_sync + 1);
    return field_clocks_twice * kCpuClock / (2 * VideoClock());
}

uint64_t System::LegacyFieldTicksSingle() const
{
    // The legacy timing's length of a field after a write of V_SYNC, computed in single precision.
    const auto rate = static_cast<float>(FieldRate());
    const uint64_t lines = LegacyLines();
    const float line = static_cast<float>(VideoClock()) / rate / static_cast<float>(lines);
    return 2 * lines * static_cast<uint64_t>(line);
}

uint64_t System::LegacyLines() const
{
    return (vi_.registers[Vi::kVSync] & Vi::kLineMask) + 1;
}

uint64_t System::ViInterruptOffset() const
{
    // The interrupt comes at the half-line that V_INTR names. The legacy players raise it at the start of the field.
    if (options_.timing == Timing::kLegacy)
    {
        return 0;
    }

    const uint64_t v_sync = VSyncOrDefault();
    const uint64_t v_intr = std::min<uint64_t>(vi_.registers[Vi::kIntr] & Vi::kLineMask, v_sync);
    return vi_.field_ticks * v_intr / (v_sync + 1);
}

int64_t System::FieldStartAt(uint64_t time) const
{
    // The field that holds the next interrupt starts V_INTR half-lines before it. The current field may be the one
    // before.
    const int64_t next = static_cast<int64_t>(vi_.next_interrupt);
    int64_t start = next - static_cast<int64_t>(ViInterruptOffset());
    if (start > static_cast<int64_t>(time))
    {
        start -= static_cast<int64_t>(vi_.field_ticks);
    }

    return start;
}

uint32_t System::LegacyViCurrent(uint64_t time) const
{
    uint32_t line = static_cast<uint32_t>(ElapsedInField(time) / kLegacyTicksPerViLine) & Vi::kLineMask;
    if ((vi_.registers[Vi::kStatus] & kViSerrate) != 0)
    {
        line = (line & ~1u) | vi_.field;
    }

    return line;
}

uint64_t System::LegacyViCurrentChangeTime(uint64_t time) const
{
    const uint64_t elapsed = ElapsedInField(time);
    const uint64_t next_line = time - elapsed % kLegacyTicksPerViLine + kLegacyTicksPerViLine;

    // A new field starts at the next interrupt. That need not be the end of a half-line.
    return vi_.next_interrupt > time ? std::min(next_line, vi_.next_interrupt) : next_line;
}

void System::StartVi()
{
    vi_.field_ticks = FieldTicks();
    vi_.field = 0;
    vi_.next_interrupt = now_ + ViInterruptOffset();
    if (vi_.next_interrupt <= now_)
    {
        vi_.next_interrupt += vi_.field_ticks;
    }

    Schedule(Event::kVi, vi_.next_interrupt);
}

void System::ScheduleVi()
{
    // The field's length or the interrupt's half-line changed. The current field keeps its start.
    const int64_t start = FieldStartAt(now_);
    vi_.field_ticks = FieldTicks();
    int64_t next = start + static_cast<int64_t>(ViInterruptOffset());
    while (next <= static_cast<int64_t>(now_))
    {
        next += static_cast<int64_t>(vi_.field_ticks);
    }

    vi_.next_interrupt = static_cast<uint64_t>(next);
    Schedule(Event::kVi, vi_.next_interrupt);
}

void System::FinishViField()
{
    // On the hardware, the interrupt never comes when V_INTR is beyond the field's last half-line.
    const uint32_t v_intr = vi_.registers[Vi::kIntr] & Vi::kLineMask;
    const uint32_t v_sync = vi_.registers[Vi::kVSync] & Vi::kLineMask;
    if (options_.timing == Timing::kLegacy || (v_sync != 0 && v_intr <= v_sync))
    {
        RaiseMi(kMiInterruptVi);
    }

    if ((vi_.registers[Vi::kStatus] & kViSerrate) != 0)
    {
        vi_.field ^= 1;
    }

    vi_.fields++;
    if (LateEvents())
    {
        // The next field counts from the moment this one's interrupt was handled.
        vi_.next_interrupt = now_ + vi_.field_ticks;
    }
    else
    {
        vi_.field_ticks = FieldTicks();
        vi_.next_interrupt += vi_.field_ticks;
    }

    Schedule(Event::kVi, vi_.next_interrupt);
}

uint32_t System::ReadVi(uint32_t index)
{
    if (index >= Vi::kRegisterCount)
    {
        return 0;
    }
    if (index != Vi::kCurrent)
    {
        return vi_.registers[index];
    }

    // VI_V_CURRENT: the half-line being scanned, with bit 0 giving the field in the interlaced modes.
    if (options_.timing == Timing::kLegacy)
    {
        return LegacyViCurrent(now_);
    }

    const uint64_t elapsed = ElapsedInField(now_);
    const uint64_t v_sync = VSyncOrDefault();
    uint32_t line = static_cast<uint32_t>(elapsed * (v_sync + 1) / std::max<uint64_t>(vi_.field_ticks, 1));
    line &= Vi::kLineMask;
    if ((vi_.registers[Vi::kStatus] & kViSerrate) != 0)
    {
        line = (line & ~1u) | vi_.field;
    }

    return line;
}

void System::WriteVi(uint32_t index, uint32_t value)
{
    static constexpr std::array<uint32_t, Vi::kRegisterCount> kMasks = {
        0x0000FFFF, 0x00FFFFFF, 0x00000FFF, 0x000003FF, 0x000003FF, 0x3FFFFFFF, 0x000003FF,
        0x001FFFFF, 0x0FFFFFFF, 0x03FF03FF, 0x03FF03FF, 0x03FF03FF, 0x0FFF0FFF, 0x0FFF0FFF,
    };

    if (index >= Vi::kRegisterCount)
    {
        return;
    }
    if (index == Vi::kCurrent)
    {
        // A write to VI_V_CURRENT acknowledges the interrupt.
        LowerMi(kMiInterruptVi);
        return;
    }

    vi_.registers[index] = value & kMasks[index];

    if (LateEvents())
    {
        // A write of V_SYNC changes the field's length. A scheduled interrupt keeps its time.
        if (index == Vi::kVSync)
        {
            vi_.field_ticks = LegacyFieldTicksSingle();
        }
        return;
    }

    if (index == Vi::kIntr || index == Vi::kVSync || index == Vi::kHSync)
    {
        ScheduleVi();
    }
}

// AI.

uint32_t System::AiStatus() const
{
    uint32_t status = 0;
    if (ai_.count > 1)
    {
        status |= kAiStatusFull;
    }
    if (ai_.count > 0)
    {
        status |= kAiStatusBusy;
    }
    if ((ai_.control & kAiControlDma) != 0)
    {
        status |= kAiStatusEnabled;
    }

    return status;
}

bool System::AiDelay() const
{
    return options_.timing != Timing::kLegacy || options_.ai_delay;
}

bool System::RcpDelays() const
{
    return options_.timing != Timing::kLegacy || options_.rcp_delays;
}

uint32_t System::AudioSampleRate() const
{
    if (ai_.dac_rate == 0)
    {
        return 44100;
    }

    return static_cast<uint32_t>(VideoClock() / (ai_.dac_rate + 1));
}

uint64_t System::AiBufferTicks(uint32_t length) const
{
    if (options_.timing == Timing::kLegacy)
    {
        // length × F × field rate ÷ (4 × sample rate) count units, where F is the field's length in count units. With
        // the legacy timing, F is the length that the VI uses now. A DACRATE of 0 then gives the video clock as the
        // rate.
        const bool late = LateEvents();
        const uint64_t field = (late ? vi_.field_ticks : FieldTicks()) / 2;
        const uint64_t sample_rate = late ? VideoClock() / (ai_.dac_rate + 1) : AudioSampleRate();
        return 2 * (uint64_t{length} * field * FieldRate() / (4 * sample_rate));
    }

    // Four bytes a sample, each lasting DACRATE + 1 video clocks.
    const uint64_t samples = length / 4;
    return samples * (ai_.dac_rate + 1) * kCpuClock / VideoClock();
}

void System::StartAiBuffer()
{
    Ai::Buffer& buffer = ai_.buffers[0];
    buffer.start = now_;
    buffer.started = true;
    if (!LateEvents())
    {
        buffer.duration = std::max<uint64_t>(AiBufferTicks(buffer.length), 1);
    }

    if (AiDelay())
    {
        Schedule(Event::kAiBuffer, now_ + buffer.duration);
    }
    else
    {
        // Without the delay, the buffer ends at the next check for events: with the legacy timing, the end of the next
        // branch or jump.
        Schedule(Event::kAiBuffer, now_);
    }

    // The hardware raises the interrupt when a buffer starts, so that the next one can be queued. The legacy players
    // raise it when a buffer ends.
    if (options_.timing == Timing::kHardware)
    {
        RaiseMi(kMiInterruptAi);
    }

    // The buffer's samples, taken as it starts.
    const uint32_t frames = buffer.length / 4;
    audio_scratch_.resize(size_t{frames} * 2);
    for (uint32_t i = 0; i < frames * 2; i++)
    {
        const uint32_t address = (buffer.address + 2 * i) & kRdramAddressMask;
        uint16_t sample = 0;
        if (address + 2 <= rdram_.size())
        {
            sample = xsflib::LoadBe16(rdram_.data() + address);
        }

        audio_scratch_[i] = static_cast<int16_t>(sample);
    }

    if (Coverage* coverage = ActiveCoverage())
    {
        coverage->ReadRdram(buffer.address, buffer.length);
    }

    if (audio_callback_ && frames > 0)
    {
        audio_callback_({audio_scratch_, AudioSampleRate()});
    }
}

void System::FinishAiBuffer()
{
    if (ai_.count == 0)
    {
        return;
    }

    ai_.buffers[0] = ai_.buffers[1];
    ai_.buffers[1] = {};
    ai_.count--;

    if (options_.timing == Timing::kLegacy)
    {
        RaiseMi(kMiInterruptAi);
    }

    if (ai_.count > 0 && (ai_.control & kAiControlDma) != 0)
    {
        StartAiBuffer();
    }
}

uint32_t System::ReadAi(uint32_t index)
{
    if (index == Ai::kStatus)
    {
        return AiStatus();
    }

    // The other registers read as AI_LENGTH: the bytes left in the buffer that plays. The legacy players read 0 without
    // the delay, and for the buffer that a loaded state describes.
    if (ai_.count == 0 || !AiDelay())
    {
        return 0;
    }

    const Ai::Buffer& buffer = ai_.buffers[0];
    if (LateEvents())
    {
        // The legacy timing's readback, in count units. Count includes the reading instruction. The end may be due and
        // not handled yet.
        const uint64_t end = events_[static_cast<int>(Event::kAiBuffer)];
        if (buffer.from_state || buffer.duration == 0 || end == kNever || end <= now_)
        {
            return 0;
        }

        const uint64_t left = (end - now_) / 2;
        if (left >= 0x80000000 || static_cast<uint32_t>(cpu_->Count() + left) == 0)
        {
            return 0;
        }

        return static_cast<uint32_t>(left * buffer.length / (buffer.duration / 2)) & ~7u;
    }
    if (buffer.duration == 0 || buffer.from_state || now_ >= buffer.start + buffer.duration)
    {
        return 0;
    }

    return AiBytesLeft(buffer);
}

uint32_t System::AiBytesLeft(const Ai::Buffer& buffer) const
{
    const uint64_t left = buffer.start + buffer.duration - now_;
    return static_cast<uint32_t>(uint64_t{buffer.length} * left / buffer.duration) & ~7u;
}

void System::WriteAi(uint32_t index, uint32_t value)
{
    switch (index)
    {
    case 0:
        ai_.dram_address = value & kDmaRdramAddressMask;
        break;

    case 1:
        QueueAiBuffer(value);
        break;

    case 2:
        ai_.control = value & kAiControlDma;
        if ((ai_.control & kAiControlDma) != 0 && ai_.count > 0 && !ai_.buffers[0].started)
        {
            StartAiBuffer();
        }
        break;

    case Ai::kStatus:
        // In the legacy timing, a write of AI_STATUS re-checks the interrupt lines before it clears the latch. The bit
        // therefore stays in MI_INTR until the next re-check.
        if (LateEvents())
        {
            RecheckInterrupts();
            ai_.interrupt_latch = false;
        }
        else
        {
            LowerMi(kMiInterruptAi);
        }
        break;

    case 4:
        ai_.dac_rate = value & kAiDacRateMask;
        break;

    case 5:
        ai_.bit_rate = value & kAiBitRateMask;
        break;

    default:
        break;
    }
}

void System::QueueAiBuffer(uint32_t length)
{
    ai_.length = length & kAiLengthMask;
    if (ai_.length == 0)
    {
        return;
    }

    // The legacy timing computes a buffer's duration when the game writes AI_LEN.
    Ai::Buffer buffer{ai_.dram_address, ai_.length};
    if (LateEvents())
    {
        buffer.duration = std::max<uint64_t>(AiBufferTicks(ai_.length), 1);
    }

    if (!AiDelay())
    {
        // A buffer starts at once. It replaces one that hasn't ended yet, and the two end together.
        ai_.buffers[0] = buffer;
        ai_.count = 1;
        if ((ai_.control & kAiControlDma) != 0)
        {
            StartAiBuffer();
        }
        return;
    }
    if (ai_.count >= 2)
    {
        return;
    }

    ai_.buffers[ai_.count] = buffer;
    ai_.count++;
    if (ai_.count == 1 && (ai_.control & kAiControlDma) != 0)
    {
        StartAiBuffer();
    }
}

// PI.

uint32_t System::PiStatus() const
{
    uint32_t status = 0;
    if (pi_.busy)
    {
        status |= kPiStatusDmaBusy;
    }
    if (pi_.error)
    {
        status |= kPiStatusError;
    }
    if ((mi_.interrupt & kMiInterruptPi) != 0)
    {
        status |= kPiStatusInterrupt;
    }

    return status;
}

uint64_t System::PiTransferTicks(uint32_t cart_address, uint32_t length) const
{
    if (options_.timing == Timing::kLegacy)
    {
        // length ÷ 8 count units.
        return std::max<uint64_t>(2 * (length / 8), 1);
    }

    // Each page of the domain costs its latency, and each 16-bit word its pulse width and release time, in RCP cycles.
    // An RCP cycle is one and a half ticks.
    const bool domain2 = (cart_address >= kCartDomain2Address1 && cart_address < kCartDomain1Address1) ||
                         (cart_address >= kCartDomain2Address2 && cart_address < kCartRom);
    const uint32_t* domain = pi_.domains.data() + (domain2 ? 4 : 0);
    const uint64_t latency = domain[0] + 1;
    const uint64_t pulse = domain[1] + 1;
    const uint64_t page = uint64_t{1} << ((domain[2] & 0xF) + 2);
    const uint64_t release = domain[3] + 1;
    const uint64_t pages = (uint64_t{length} + page - 1) / page;
    const uint64_t words = (uint64_t{length} + 1) / 2;
    const uint64_t cycles = pages * latency + words * (pulse + release);
    return std::max<uint64_t>(cycles * 3 / 2, 1);
}

uint32_t System::ReadPi(uint32_t index)
{
    switch (index)
    {
    case 0:
        return pi_.dram_address;

    case 1:
        return pi_.cart_address;

    case 2:
        return pi_.read_length;

    case 3:
        return pi_.write_length;

    case 4:
        {
            uint32_t status = PiStatus();
            if (pi_.latch_valid && options_.timing == Timing::kHardware && now_ < pi_.latch_until)
            {
                status |= kPiStatusIoBusy; // IO busy: a write to cartridge space is in the latch.
            }
            return status;
        }

    default:
        if (index >= 5 && index < 13)
        {
            return pi_.domains[index - 5];
        }
        return 0;
    }
}

void System::WritePi(uint32_t index, uint32_t value)
{
    switch (index)
    {
    case 0:
        pi_.dram_address = value & kPiDramAddressMask;
        return;

    case 1:
        pi_.cart_address = value & kPiCartAddressMask;
        return;

    case 2:
    case 3:
        break;

    case 4:
        if ((value & 1) != 0)
        {
            pi_.busy = false;
            pi_.error = false;
            Cancel(Event::kPi);
        }
        if ((value & 2) != 0)
        {
            LowerMi(kMiInterruptPi);
        }
        return;

    default:
        if (index >= 5 && index < 13)
        {
            static constexpr std::array<uint32_t, 4> kMasks = {0xFF, 0xFF, 0x0F, 0x03};
            pi_.domains[index - 5] = value & kMasks[(index - 5) & 3];
        }
        return;
    }

    // A transfer: PI_RD_LEN copies from RDRAM to the cartridge, PI_WR_LEN from the cartridge to RDRAM. The length
    // register holds the length minus one.
    const bool to_rdram = index == 3;
    const uint32_t requested = (value & kPiLengthMask) + 1;
    const uint32_t cart = pi_.cart_address;
    const uint32_t dram = pi_.dram_address;
    pi_.busy = true;

    // Only ROM answers. A copy to the cartridge, or from save memory, the 64DD or the PIF, copies nothing.
    const bool touches_rom = to_rdram && cart >= kCartRom && cart < kPifRom && cart - kCartRom < rom_image_size_;
    uint32_t cart_end = 0;
    uint32_t dram_end = 0;
    if (to_rdram)
    {
        CopyPiToRdram(cart, dram, requested, cart_end, dram_end);
    }
    else
    {
        cart_end = cart + ((requested + 1) & ~1u);
        dram_end = (dram + requested + 7) & ~7u;
    }

    pi_.dram_address = dram_end & kPiDramAddressMask;
    pi_.cart_address = cart_end & kPiCartAddressMask;

    const uint32_t length = cart_end - cart;
    uint64_t ticks = PiTransferTicks(cart, length);
    if (options_.timing == Timing::kLegacy && !touches_rom)
    {
        ticks = kLegacyEmptyPiTicks;
    }

    if (!RcpDelays())
    {
        // The interrupt comes at once.
        FinishPiDma();
        return;
    }

    Schedule(Event::kPi, now_ + ticks);
}

// The PI copies in blocks of up to 128 bytes through a buffer, and no block crosses a 2 KiB page of RDRAM. Each 16-bit
// read from the bus fills two bytes, so a block of odd length copies one byte more.
//
// The first block is different. Its writes are masked to the byte, so an odd length copies exactly, unless the block
// reaches the last two bytes of the buffer. An RDRAM address that isn't a multiple of 8 puts the data at that offset in
// the buffer, which shortens the block. The hardware counts the block's length from the start of the buffer, so it
// writes `offset` bytes fewer at the end. A first block that fills the buffer still moves the RDRAM address to the
// buffer's end, which leaves a hole. When the first block ends at a page within the first 8 bytes, the next block is
// shortened by the offset too, and it leaves a hole of that size after it.
//
// n64brew describes the blocks. n64-systemtest's DMA tests give the exact results, and this follows them all.
void System::CopyPiToRdram(uint32_t cart, uint32_t dram, uint32_t length, uint32_t& cart_end, uint32_t& dram_end)
{
    constexpr uint32_t kBuffer = 128;
    uint32_t offset = dram & 7;
    uint32_t remaining = length;
    bool first = true;
    while (remaining > 0)
    {
        // The size of the block: the rest of the transfer, up to the end of the buffer or of the RDRAM page.
        const uint32_t page_left = kRdramPageSize - (dram & (kRdramPageSize - 1));
        uint32_t size = std::min(remaining, std::min(kBuffer - offset, page_left));

        // How many bytes the block writes, and how far it moves the RDRAM address.
        uint32_t written = 0;
        uint32_t advance = 0;
        if (first)
        {
            if ((size & 1) != 0 && offset + size == kBuffer - 1)
            {
                size++;
            }
            written = size > offset ? size - offset : 0;
            if (offset + size == kBuffer)
            {
                advance = size;
                offset = 0;
            }
            else if (size == page_left)
            {
                advance = size;
                if (offset + size > 8)
                {
                    offset = 0;
                }
            }
            else
            {
                advance = written;
            }
        }
        else
        {
            size = (size + 1) & ~1u;
            written = size;
            if (offset + size >= kBuffer)
            {
                advance = kBuffer;
                offset = 0;
            }
            else
            {
                advance = size;
            }
        }

        // The copy.
        for (uint32_t i = 0; i < written; i++)
        {
            CopyRomByte(cart + i, dram + i);
        }

        // The coverage: the ROM bytes that the block reads and the RDRAM bytes that it writes.
        if (Coverage* coverage = ActiveCoverage())
        {
            if (cart >= kCartRom && cart < kPifRom && cart - kCartRom < rom_image_size_)
            {
                coverage->ReadRom(cart - kCartRom, std::min(size, rom_image_size_ - (cart - kCartRom)));
            }
            if (written > 0)
            {
                coverage->WriteRdram(dram & kRdramAddressMask, written);
            }
        }

        // The next block.
        cart += (size + 1) & ~1u;
        dram += advance;
        remaining = remaining > size ? remaining - size : 0;
        first = false;
    }

    cart_end = cart;
    dram_end = (dram + 7) & ~7u;
}

void System::CopyRomByte(uint32_t cart, uint32_t dram)
{
    const uint32_t target = dram & kRdramAddressMask;
    if (cart < kCartRom || cart >= kPifRom || target >= rdram_.size())
    {
        return;
    }

    const uint32_t source = cart - kCartRom;
    if (source < rom_image_size_)
    {
        rdram_[target] = source < rom_.size() ? rom_[source] : 0;
    }
}

void System::FinishPiDma()
{
    pi_.busy = false;
    RaiseMi(kMiInterruptPi);
}

// SI.

uint32_t System::SiStatus() const
{
    uint32_t status = 0;
    if (si_.busy)
    {
        status |= kSiStatusDmaBusy;
    }
    if ((mi_.interrupt & kMiInterruptSi) != 0)
    {
        status |= kSiStatusInterrupt;
    }

    return status;
}

uint32_t System::ReadSi(uint32_t index)
{
    switch (index)
    {
    case 0:
        return si_.dram_address;

    case 1:
        return si_.pif_address_read;

    case 4:
        return si_.pif_address_write;

    case 6:
        return SiStatus();

    default:
        return 0;
    }
}

void System::WriteSi(uint32_t index, uint32_t value)
{
    switch (index)
    {
    case 0:
        si_.dram_address = value & kDmaRdramAddressMask;
        break;

    case 1:
        si_.pif_address_read = value;
        StartSiDma(true);
        break;

    case 4:
        si_.pif_address_write = value;
        StartSiDma(false);
        break;

    case 6:
        LowerMi(kMiInterruptSi);
        break;

    default:
        break;
    }
}

void System::StartSiDma(bool to_rdram)
{
    // The 64 bytes of PIF RAM move at once. The PIF runs its commands before a read and after a write.
    auto& ram = pif_.Ram();
    const uint32_t dram = si_.dram_address;
    if (to_rdram)
    {
        pif_.OnRead(now_, vi_.fields);
        DmaWriteRdram(dram, ram.data(), kPifRamSize);
    }
    else
    {
        DmaReadRdram(dram, ram.data(), kPifRamSize);
        pif_.OnWrite();
    }

    si_.busy = true;
    if (!RcpDelays())
    {
        // The transfer takes effect at once.
        FinishSiDma();
        return;
    }

    Schedule(Event::kSi, now_ + kSiTransferTicks);
}

void System::FinishSiDma()
{
    si_.busy = false;
    RaiseMi(kMiInterruptSi);
}

// RI.

uint32_t System::ReadRi(uint32_t index)
{
    if (index == 6 || index == 7)
    {
        return 0;
    }

    return ri_.registers[index & 7];
}

void System::WriteRi(uint32_t index, uint32_t value)
{
    static constexpr std::array<uint32_t, 8> kMasks = {0xF, 0x7F, 0xFFFFFFFF, 0xFF, 0x7FFFF, 0xF, 0, 0};
    ri_.registers[index & 7] = value & kMasks[index & 7];
}

// DP.

uint32_t System::ReadDp(uint32_t index)
{
    switch (index & 7)
    {
    case 0:
        return dp_.start;
    case 1:
        return dp_.end;
    case 2:
        return dp_.current;
    case 3:
        return dp_.status;
    case 4:
        return dp_.clock;
    case 5:
        return dp_.buffer_busy;
    case 6:
        return dp_.pipe_busy;
    default:
        return dp_.tmem;
    }
}

void System::WriteDp(uint32_t index, uint32_t value)
{
    switch (index & 7)
    {
    case 0:
        if ((dp_.status & kDpStartValid) == 0)
        {
            dp_.start = value & kDmaRdramAddressMask;
            dp_.status |= kDpStartValid;
        }
        break;

    case 1:
        dp_.end = value & kDmaRdramAddressMask;
        if ((dp_.status & kDpStartValid) != 0)
        {
            dp_.current = dp_.start;
            dp_.status &= ~kDpStartValid;
        }
        RunDpCommands();
        break;

    case 3:
        WriteDpStatus(value);
        break;

    default:
        break;
    }
}

void System::WriteDpStatus(uint32_t value)
{
    // XBUS, freeze and flush each have a clear bit and the set bit above it, from bit 0.
    const bool was_frozen = (dp_.status & kDpFreeze) != 0;
    ApplyClearSet(dp_.status, value, 0, 1, kDpXbus);
    ApplyClearSet(dp_.status, value, 2, 3, kDpFreeze);
    ApplyClearSet(dp_.status, value, 4, 5, kDpFlush);

    if ((value & kDpWriteClearTmem) != 0)
    {
        dp_.tmem = 0;
    }
    if ((value & kDpWriteClearPipe) != 0)
    {
        dp_.pipe_busy = 0;
    }
    if ((value & kDpWriteClearCommand) != 0)
    {
        dp_.buffer_busy = 0;
    }
    if ((value & kDpWriteClearClock) != 0)
    {
        dp_.clock = 0;
    }

    if (was_frozen && (dp_.status & kDpFreeze) == 0)
    {
        RunDpCommands();
    }
}

void System::RunDpCommands()
{
    // The RDP doesn't draw. It walks the commands from CURRENT to END and raises its interrupt at each full sync.
    if ((dp_.status & kDpFreeze) != 0)
    {
        return;
    }

    const bool from_dmem = (dp_.status & kDpXbus) != 0;
    const auto read_word = [this, from_dmem](uint32_t address) -> uint32_t
    {
        if (from_dmem)
        {
            return xsflib::LoadBe32(rsp_->Dmem().data() + (address & Rsp::kDoubleWordAddressMask));
        }

        address &= kDmaRdramAddressMask;
        return address + 4 <= rdram_.size() ? xsflib::LoadBe32(rdram_.data() + address) : 0;
    };

    uint32_t current = dp_.current;
    while (current < dp_.end)
    {
        const uint32_t op = (read_word(current) >> 24) & 0x3F;
        const uint32_t length = DpCommandLength(op);
        if (current + length > dp_.end)
        {
            // The rest of the command hasn't arrived yet.
            break;
        }

        if (op == kDpFullSync)
        {
            RaiseMi(kMiInterruptDp);
        }

        current += length;
    }

    dp_.current = current;
    dp_.status = (dp_.status & ~kDpEndValid) | kDpCommandBufferReady;
}

} // namespace usf::n64
