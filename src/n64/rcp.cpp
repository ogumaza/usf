// SPDX-License-Identifier: MIT

// The RCP's interfaces: MI, VI, AI, PI, SI, RI, the RDRAM chips' registers and the DP's command registers.

#include <algorithm>

#include "xsflib/bytes.h"

#include "n64/system.h"

namespace usf::n64
{

namespace
{

// DPC_STATUS bits.
constexpr uint32_t kDpXbus = 1u << 0;
constexpr uint32_t kDpFreeze = 1u << 1;
constexpr uint32_t kDpFlush = 1u << 2;
constexpr uint32_t kDpCommandBufferReady = 1u << 7;
constexpr uint32_t kDpEndValid = 1u << 9;
constexpr uint32_t kDpStartValid = 1u << 10;

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

// The current-control value in a chip's MODE register: six bits spread over the register, stored inverted.
uint32_t DecodeCurrent(uint32_t mode)
{
    uint32_t value = ((mode >> 6) & 1) | (((mode >> 14) & 1) << 1) | (((mode >> 22) & 1) << 2) |
                     (((mode >> 7) & 1) << 3) | (((mode >> 15) & 1) << 4) | (((mode >> 23) & 1) << 5);
    return value ^ 0x3F;
}

uint32_t EncodeCurrent(uint32_t value)
{
    value ^= 0x3F;
    return ((value & 1) << 6) | (((value >> 1) & 1) << 14) | (((value >> 2) & 1) << 22) | (((value >> 3) & 1) << 7) |
           (((value >> 4) & 1) << 15) | (((value >> 5) & 1) << 23);
}

// A chip returns its data intact at a current-control value of kRdramCurrentHigh or more, and none of it at
// kRdramCurrentLow or less. Between the two, each set bit survives with a probability that rises with the current.
// IPL3 finds the current it sets by watching the bits. The two values lie within the ranges that chips show.
constexpr uint32_t kRdramCurrentLow = 9;
constexpr uint32_t kRdramCurrentHigh = 15;
constexpr uint32_t kRdramChipSize = 0x200000;

uint32_t DecodeDeviceId(uint32_t value)
{
    return ((value >> 26) & 0x3F) | (((value >> 23) & 1) << 6) | (((value >> 8) & 0xFF) << 7) |
           (((value >> 7) & 1) << 15);
}

} // namespace

// MI.

uint32_t System::ReadMi(uint32_t index)
{
    switch (index)
    {
    case 0:
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
    if (index == 0)
    {
        // MI_MODE: the repeat length, and clear and set bits for init mode, ebus test mode and RDRAM register mode.
        uint32_t mode = (mi_.mode & ~0x7Fu) | (value & 0x7F);
        auto apply = [&mode, value](int clear_bit, int set_bit, uint32_t flag)
        {
            if ((value & (1u << clear_bit)) != 0)
            {
                mode &= ~flag;
            }
            if ((value & (1u << set_bit)) != 0)
            {
                mode |= flag;
            }
        };

        apply(7, 8, 1u << 7);
        apply(9, 10, 1u << 8);
        apply(12, 13, 1u << 9);
        mi_.mode = mode;

        if ((value & (1u << 11)) != 0)
        {
            LowerMi(kMiInterruptDp);
        }
        return;
    }

    if (index == 3)
    {
        // MI_MASK: a clear bit and a set bit for each interrupt.
        for (int i = 0; i < 6; i++)
        {
            if ((value & (1u << (2 * i))) != 0)
            {
                mi_.mask &= ~(1u << i);
            }
            if ((value & (1u << (2 * i + 1))) != 0)
            {
                mi_.mask |= 1u << i;
            }
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

uint64_t System::FieldTicks() const
{
    const bool pal = tv_ == TvType::kPal;
    uint64_t v_sync = vi_.registers[6] & 0x3FF;
    if (options_.timing == Timing::kLegacy)
    {
        // (V_SYNC + 1) × L count units, where L is the video clock over the field rate over (V_SYNC + 1), rounded
        // down. A count unit is two ticks.
        const uint64_t rate = pal ? 50 : 60;
        const uint64_t lines = v_sync + 1;
        return 2 * lines * (VideoClock() / rate / lines);
    }

    // V_SYNC + 1 half-lines, each half of (H_SYNC + 1) video clocks.
    uint64_t h_sync = vi_.registers[7] & 0xFFF;
    if (v_sync == 0)
    {
        v_sync = pal ? kDefaultVSyncPal : kDefaultVSyncNtsc;
    }
    if (h_sync == 0)
    {
        h_sync = pal ? kDefaultHSyncPal : kDefaultHSyncNtsc;
    }

    const uint64_t field_clocks_twice = (v_sync + 1) * (h_sync + 1);
    return field_clocks_twice * kCpuClock / (2 * VideoClock());
}

uint64_t System::LegacyFieldTicksSingle() const
{
    // The legacy timing's length of a field after a write of V_SYNC, computed in single precision.
    const float rate = tv_ == TvType::kPal ? 50.0f : 60.0f;
    const uint64_t lines = (vi_.registers[6] & 0x3FF) + 1;
    const float line = static_cast<float>(VideoClock()) / rate / static_cast<float>(lines);
    return 2 * lines * static_cast<uint64_t>(line);
}

uint64_t System::ViInterruptOffset() const
{
    // The interrupt comes at the half-line that V_INTR names. The legacy players raise it at the start of the field.
    if (options_.timing == Timing::kLegacy)
    {
        return 0;
    }

    uint64_t v_sync = vi_.registers[6] & 0x3FF;
    if (v_sync == 0)
    {
        v_sync = tv_ == TvType::kPal ? kDefaultVSyncPal : kDefaultVSyncNtsc;
    }

    const uint64_t v_intr = std::min<uint64_t>(vi_.registers[3] & 0x3FF, v_sync);
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
    const uint64_t elapsed =
        static_cast<uint64_t>(std::max<int64_t>(static_cast<int64_t>(time) - FieldStartAt(time), 0));
    uint32_t line = static_cast<uint32_t>(elapsed / kLegacyTicksPerViLine) & 0x3FF;
    if ((vi_.registers[0] & kViSerrate) != 0)
    {
        line = (line & ~1u) | vi_.field;
    }

    return line;
}

uint64_t System::LegacyViCurrentChangeTime(uint64_t time) const
{
    const uint64_t elapsed =
        static_cast<uint64_t>(std::max<int64_t>(static_cast<int64_t>(time) - FieldStartAt(time), 0));
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
    const uint32_t v_intr = vi_.registers[3] & 0x3FF;
    const uint32_t v_sync = vi_.registers[6] & 0x3FF;
    if (options_.timing == Timing::kLegacy || (v_sync != 0 && v_intr <= v_sync))
    {
        RaiseMi(kMiInterruptVi);
    }

    if ((vi_.registers[0] & kViSerrate) != 0)
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
    if (index >= 14)
    {
        return 0;
    }
    if (index != 4)
    {
        return vi_.registers[index];
    }

    // VI_V_CURRENT: the half-line being scanned, with bit 0 giving the field in the interlaced modes.
    if (options_.timing == Timing::kLegacy)
    {
        return LegacyViCurrent(now_);
    }

    const uint64_t elapsed =
        static_cast<uint64_t>(std::max<int64_t>(static_cast<int64_t>(now_) - FieldStartAt(now_), 0));
    uint64_t v_sync = vi_.registers[6] & 0x3FF;
    if (v_sync == 0)
    {
        v_sync = tv_ == TvType::kPal ? kDefaultVSyncPal : kDefaultVSyncNtsc;
    }

    uint32_t line = static_cast<uint32_t>(elapsed * (v_sync + 1) / std::max<uint64_t>(vi_.field_ticks, 1));
    line &= 0x3FF;
    if ((vi_.registers[0] & kViSerrate) != 0)
    {
        line = (line & ~1u) | vi_.field;
    }

    return line;
}

void System::WriteVi(uint32_t index, uint32_t value)
{
    static constexpr std::array<uint32_t, 14> kMasks = {
        0x0000FFFF, 0x00FFFFFF, 0x00000FFF, 0x000003FF, 0x000003FF, 0x3FFFFFFF, 0x000003FF,
        0x001FFFFF, 0x0FFFFFFF, 0x03FF03FF, 0x03FF03FF, 0x03FF03FF, 0x0FFF0FFF, 0x0FFF0FFF,
    };

    if (index >= 14)
    {
        return;
    }
    if (index == 4)
    {
        // A write to VI_V_CURRENT acknowledges the interrupt.
        LowerMi(kMiInterruptVi);
        return;
    }

    vi_.registers[index] = value & kMasks[index];

    if (LateEvents())
    {
        // A write of V_SYNC changes the field's length. A scheduled interrupt keeps its time.
        if (index == 6)
        {
            vi_.field_ticks = LegacyFieldTicksSingle();
            if (events_[static_cast<int>(Event::kVi)] == kNever && vi_.registers[6] != 0)
            {
                vi_.next_interrupt = now_ + vi_.field_ticks;
                Schedule(Event::kVi, vi_.next_interrupt);
            }
        }
        return;
    }

    if (index == 3 || index == 6 || index == 7)
    {
        ScheduleVi();
    }
}

// AI.

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
        const uint64_t rate = tv_ == TvType::kPal ? 50 : 60;
        const bool late = LateEvents();
        const uint64_t field = (late ? vi_.field_ticks : FieldTicks()) / 2;
        const uint64_t sample_rate = late ? VideoClock() / (ai_.dac_rate + 1) : AudioSampleRate();
        return 2 * (uint64_t{length} * field * rate / (4 * sample_rate));
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
        const uint32_t address = (buffer.address + 2 * i) & 0xFFFFFF;
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

    if (ai_.count > 0 && (ai_.control & 1) != 0)
    {
        StartAiBuffer();
    }
}

uint32_t System::ReadAi(uint32_t index)
{
    if (index == 3)
    {
        uint32_t status = 0;
        if (ai_.count > 1)
        {
            status |= 0x80000001;
        }
        if (ai_.count > 0)
        {
            status |= 0x40000000;
        }
        if ((ai_.control & 1) != 0)
        {
            status |= 0x02000000;
        }
        return status;
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

    const uint64_t left = buffer.start + buffer.duration - now_;
    return static_cast<uint32_t>(uint64_t{buffer.length} * left / buffer.duration) & ~7u;
}

void System::WriteAi(uint32_t index, uint32_t value)
{
    switch (index)
    {
    case 0:
        ai_.dram_address = value & 0xFFFFF8;
        break;

    case 1:
        {
            ai_.length = value & 0x3FFF8;
            if (ai_.length == 0)
            {
                break;
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
                if ((ai_.control & 1) != 0)
                {
                    StartAiBuffer();
                }
                break;
            }
            if (ai_.count >= 2)
            {
                break;
            }

            ai_.buffers[ai_.count] = buffer;
            ai_.count++;
            if (ai_.count == 1 && (ai_.control & 1) != 0)
            {
                StartAiBuffer();
            }
            break;
        }

    case 2:
        ai_.control = value & 1;
        if ((ai_.control & 1) != 0 && ai_.count > 0 && !ai_.buffers[0].started)
        {
            StartAiBuffer();
        }
        break;

    case 3:
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
        ai_.dac_rate = value & 0x3FFF;
        break;

    case 5:
        ai_.bit_rate = value & 0xF;
        break;

    default:
        break;
    }
}

// PI.

uint64_t System::PiTransferTicks(uint32_t cart_address, uint32_t length) const
{
    if (options_.timing == Timing::kLegacy)
    {
        // length ÷ 8 count units.
        return std::max<uint64_t>(2 * (length / 8), 1);
    }

    // Each page of the domain costs its latency, and each 16-bit word its pulse width and release time, in RCP
    // cycles. An RCP cycle is one and a half ticks.
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
            uint32_t status = 0;
            if (pi_.busy)
            {
                status |= 1;
            }
            if (pi_.latch_valid && options_.timing == Timing::kHardware && now_ < pi_.latch_until)
            {
                status |= 2; // IO busy: a write to cartridge space is in the latch.
            }
            if (pi_.error)
            {
                status |= 4;
            }
            if ((mi_.interrupt & kMiInterruptPi) != 0)
            {
                status |= 8;
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
        pi_.dram_address = value & 0xFFFFFE;
        return;

    case 1:
        pi_.cart_address = value & 0xFFFFFFFE;
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
    const uint32_t requested = (value & 0xFFFFFF) + 1;
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

    pi_.dram_address = dram_end & 0xFFFFFE;
    pi_.cart_address = cart_end & 0xFFFFFFFE;

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
// reaches the last two bytes of the buffer. An RDRAM address that isn't a multiple of 8 puts the data at that offset
// in the buffer, which shortens the block. The hardware counts the block's length from the start of the buffer, so it
// writes `offset` bytes fewer at the end. A first block that fills the buffer still moves the RDRAM address to the
// buffer's end, which leaves a hole. When the first block ends at a page within the first 8 bytes, the next block is
// shortened by the offset too, and it leaves a hole of that size after it.
//
// n64brew describes the blocks. n64-systemtest's DMA tests give the exact results, and this follows them all.
void System::CopyPiToRdram(uint32_t cart, uint32_t dram, uint32_t length, uint32_t& cart_end, uint32_t& dram_end)
{
    constexpr uint32_t kBuffer = 128;
    constexpr uint32_t kPage = 0x800;
    uint32_t offset = dram & 7;
    uint32_t remaining = length;
    bool first = true;
    while (remaining > 0)
    {
        const uint32_t page_left = kPage - (dram & (kPage - 1));
        uint32_t size = std::min(remaining, std::min(kBuffer - offset, page_left));
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

        for (uint32_t i = 0; i < written; i++)
        {
            CopyRomByte(cart + i, dram + i);
        }
        if (Coverage* coverage = ActiveCoverage())
        {
            if (cart >= kCartRom && cart < kPifRom && cart - kCartRom < rom_image_size_)
            {
                coverage->ReadRom(cart - kCartRom, std::min(size, rom_image_size_ - (cart - kCartRom)));
            }
            if (written > 0)
            {
                coverage->WriteRdram(dram & 0xFFFFFF, written);
            }
        }

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
    const uint32_t target = dram & 0xFFFFFF;
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
        {
            uint32_t status = 0;
            if (si_.busy)
            {
                status |= 1;
            }
            if ((mi_.interrupt & kMiInterruptSi) != 0)
            {
                status |= 1u << 12;
            }
            return status;
        }

    default:
        return 0;
    }
}

void System::WriteSi(uint32_t index, uint32_t value)
{
    switch (index)
    {
    case 0:
        si_.dram_address = value & 0xFFFFF8;
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

// The RDRAM chips' registers. Bit 19 of the address broadcasts a write to every chip. Bits 10 to 18 select a chip
// by its device ID. A chip that isn't enabled yet answers only when it's the first such chip, which lets IPL3 give
// each chip its ID in turn.

System::RdramChip* System::SelectRdramChip(uint32_t select)
{
    bool seen_disabled = false;
    for (RdramChip& chip : rdram_chips_)
    {
        if (!chip.present)
        {
            continue;
        }

        const bool match = (chip.device_id & ~1u) == (select & ~1u);
        if (chip.enable)
        {
            if (match)
            {
                return &chip;
            }
        }
        else if (!seen_disabled)
        {
            seen_disabled = true;
            if (match)
            {
                return &chip;
            }
        }
    }

    return nullptr;
}

uint32_t System::ReadRdramChipRegister(const RdramChip& chip, uint32_t index) const
{
    if (!chip.enable)
    {
        return 0;
    }

    switch (index)
    {
    case 0:
        return chip.device_type;

    case 1:
        return chip.device_id_register;

    case 2:
        return chip.delay | 0x03030203;

    case 3:
        {
            const uint32_t current = chip.auto_current ? chip.current_internal : DecodeCurrent(chip.mode);
            return ((chip.mode & ~0x00C0C0C0u) | EncodeCurrent(current)) ^ 0xC0C0C0C0u;
        }

    case 4:
        return chip.ref_interval;

    case 5:
        return chip.ref_row;

    case 6:
        return chip.ras_interval;

    case 7:
        return chip.min_interval;

    case 8:
        return chip.address_select;

    case 9:
        return chip.device_manufacturer;

    default:
        return 0;
    }
}

void System::WriteRdramChipRegister(RdramChip& chip, uint32_t index, uint32_t value)
{
    if (index >= 10)
    {
        return;
    }

    // Until the DELAY register sets a write delay of one, a write takes effect only when the MI repeats it (init
    // mode with a length of 16 bytes or more), and its halfwords arrive swapped.
    if (chip.write_delay != 1)
    {
        const bool init_mode = (mi_.mode & kMiModeRepeat) != 0;
        const uint32_t repeat = init_mode ? (mi_.mode & 0x7F) + 1 : 0;
        if (repeat < 16)
        {
            return;
        }

        value = (value << 16) | (value >> 16);
    }

    switch (index)
    {
    case 1:
        chip.device_id_register = value;
        chip.device_id = DecodeDeviceId(value);
        break;

    case 2:
        chip.delay = value & 0x38381838;
        chip.write_delay = (chip.delay >> 3) & 7;
        break;

    case 3:
        chip.mode = value;
        chip.enable = (value & (1u << 25)) != 0;
        chip.auto_current = (value & (1u << 31)) != 0;
        chip.current = DecodeCurrent(value);
        if (chip.auto_current)
        {
            chip.current_internal = chip.current;
        }
        break;

    case 4:
        chip.ref_interval = value;
        break;

    case 5:
        chip.ref_row = value;
        break;

    case 6:
        chip.ras_interval = value;
        break;

    case 7:
        chip.min_interval = value;
        break;

    case 8:
        chip.address_select = value;
        break;

    default:
        break;
    }
}

uint32_t System::ReadRdramRegister(uint32_t paddr)
{
    const bool broadcast = ((paddr >> 19) & 1) != 0;
    const uint32_t select = (paddr >> 10) & 0x1FF;
    const uint32_t offset = paddr & 0x3FF;
    if (broadcast)
    {
        return 0;
    }

    RdramChip* chip = SelectRdramChip(select);
    if (chip == nullptr)
    {
        return 0;
    }
    if (offset >= 0x200)
    {
        return chip->row;
    }

    return ReadRdramChipRegister(*chip, (offset >> 2) & 15);
}

void System::WriteRdramRegister(uint32_t paddr, uint32_t value)
{
    const bool broadcast = ((paddr >> 19) & 1) != 0;
    const uint32_t select = (paddr >> 10) & 0x1FF;
    const uint32_t offset = paddr & 0x3FF;
    const uint32_t index = (offset >> 2) & 15;
    if (broadcast)
    {
        for (RdramChip& chip : rdram_chips_)
        {
            if (!chip.present)
            {
                continue;
            }

            if (offset >= 0x200)
            {
                chip.row = value;
            }
            else
            {
                WriteRdramChipRegister(chip, index, value);
            }
        }

        // Repeat mode lasts for one write to the RDRAM's bus.
        mi_.mode &= ~kMiModeRepeat;
        UpdateRdramMapping();
        return;
    }

    RdramChip* chip = SelectRdramChip(select);
    if (chip == nullptr)
    {
        return;
    }
    if (offset >= 0x200)
    {
        chip->row = value;
        return;
    }

    WriteRdramChipRegister(*chip, index, value);
    mi_.mode &= ~kMiModeRepeat;
    UpdateRdramMapping();
}

// The chips at power-on, or as IPL3 leaves them: enabled, with device ID 2n, a write delay of one and a current that
// keeps their data.
void System::ResetRdramChips(bool configured)
{
    const size_t chips = options_.rdram_size > kRdramSize4 ? 4 : 2;
    for (size_t n = 0; n < rdram_chips_.size(); n++)
    {
        RdramChip& chip = rdram_chips_[n];
        chip = {};
        chip.present = n < chips;
        if (configured)
        {
            chip.enable = true;
            chip.device_id = static_cast<uint32_t>(2 * n);
            chip.write_delay = 1;
            chip.current = 63;
        }
    }
    UpdateRdramMapping();
}

// RDRAM is plain memory once chip n is enabled with device ID 2n and a current high enough to keep its data. Until
// then, each access finds its chip by device ID, and reads lose bits while the current is too low.
void System::UpdateRdramMapping()
{
    bool plain = true;
    for (size_t n = 0; n < rdram_chips_.size(); n++)
    {
        const RdramChip& chip = rdram_chips_[n];
        if (chip.present && (!chip.enable || chip.device_id != 2 * n || chip.current < kRdramCurrentHigh))
        {
            plain = false;
            break;
        }
    }

    const uint32_t size = plain ? static_cast<uint32_t>(rdram_.size()) : 0;
    if (size != direct_rdram_size_)
    {
        direct_rdram_size_ = size;
        cpu_->InvalidateFetchCache();
    }
}

uint32_t System::ReadMappedRdram32(uint32_t paddr)
{
    for (size_t n = 0; n < rdram_chips_.size(); n++)
    {
        const RdramChip& chip = rdram_chips_[n];
        if (!chip.present || !chip.enable || (chip.device_id >> 1) != (paddr >> 21))
        {
            continue;
        }

        const uint32_t offset = static_cast<uint32_t>(n) * kRdramChipSize + (paddr & (kRdramChipSize - 1));
        if (offset + 4 > rdram_.size())
        {
            return 0;
        }

        if (Coverage* coverage = ActiveCoverage())
        {
            coverage->ReadRdram(offset, 4);
        }

        const uint32_t value = xsflib::LoadBe32(rdram_.data() + offset);
        if (chip.current >= kRdramCurrentHigh)
        {
            return value;
        }
        if (chip.current <= kRdramCurrentLow)
        {
            return 0;
        }

        // A fixed xorshift sequence decides which bits survive, so that every run gives the same result.
        const uint32_t chance = (chip.current - kRdramCurrentLow) * 256 / (kRdramCurrentHigh - kRdramCurrentLow);
        uint32_t result = 0;
        for (int bit = 0; bit < 32; bit++)
        {
            rdram_noise_ ^= rdram_noise_ << 13;
            rdram_noise_ ^= rdram_noise_ >> 17;
            rdram_noise_ ^= rdram_noise_ << 5;
            if (((value >> bit) & 1) != 0 && (rdram_noise_ & 255) < chance)
            {
                result |= 1u << bit;
            }
        }
        return result;
    }

    return 0;
}

// In repeat mode, one CPU write to RDRAM repeats an 8-byte pattern over the repeat length, counted from the 8-byte word
// that the write addresses and wrapping within its 2 KiB page. Bytes before the write's address keep their values. A
// doubleword store gives the pattern. A smaller store gives the word that it puts on the bus, twice. The mode then
// turns itself off. n64brew describes the mode, and n64-systemtest's MI tests give the details.
void System::WriteRdramRepeated(uint32_t paddr, uint64_t value, int size)
{
    std::array<uint8_t, 8> pattern{};
    if (size == 8)
    {
        xsflib::StoreBe64(pattern.data(), value);
    }
    else
    {
        uint32_t word = static_cast<uint32_t>(value);
        if (size == 1)
        {
            word = static_cast<uint32_t>(value << (8 * (3 - (paddr & 3))));
        }
        else if (size == 2)
        {
            word = static_cast<uint32_t>(value << (8 * (2 - (paddr & 2))));
        }
        xsflib::StoreBe32(pattern.data(), word);
        xsflib::StoreBe32(pattern.data() + 4, word);
    }

    mi_.mode &= ~kMiModeRepeat;
    const uint32_t length = (mi_.mode & 0x7F) + 1;
    const uint32_t base = paddr & ~7u;
    const uint32_t page = paddr & ~0x7FFu;
    for (uint32_t i = paddr - base; i < length; i++)
    {
        const uint32_t address = page | ((base + i) & 0x7FF);
        if (address < direct_rdram_size_)
        {
            rdram_[address] = pattern[i & 7];
            if (Coverage* coverage = ActiveCoverage())
            {
                coverage->WriteRdram(address, 1);
            }
        }
        else
        {
            WriteMappedRdram(address, pattern[i & 7], 1);
        }
    }
}

void System::WriteMappedRdram(uint32_t paddr, uint64_t value, int size)
{
    for (size_t n = 0; n < rdram_chips_.size(); n++)
    {
        const RdramChip& chip = rdram_chips_[n];
        if (!chip.present || !chip.enable || (chip.device_id >> 1) != (paddr >> 21))
        {
            continue;
        }

        const uint32_t offset = static_cast<uint32_t>(n) * kRdramChipSize + (paddr & (kRdramChipSize - 1));
        if (offset + size > rdram_.size())
        {
            return;
        }

        uint8_t* p = rdram_.data() + offset;
        for (int i = size - 1; i >= 0; i--)
        {
            p[i] = static_cast<uint8_t>(value);
            value >>= 8;
        }
        if (Coverage* coverage = ActiveCoverage())
        {
            coverage->WriteRdram(offset, static_cast<uint32_t>(size));
        }
        return;
    }
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
            dp_.start = value & 0xFFFFF8;
            dp_.status |= kDpStartValid;
        }
        break;

    case 1:
        dp_.end = value & 0xFFFFF8;
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
    auto apply = [this, value](int clear_bit, int set_bit, uint32_t flag)
    {
        if ((value & (1u << clear_bit)) != 0)
        {
            dp_.status &= ~flag;
        }
        if ((value & (1u << set_bit)) != 0)
        {
            dp_.status |= flag;
        }
    };

    const bool was_frozen = (dp_.status & kDpFreeze) != 0;
    apply(0, 1, kDpXbus);
    apply(2, 3, kDpFreeze);
    apply(4, 5, kDpFlush);

    if ((value & (1u << 6)) != 0)
    {
        dp_.tmem = 0;
    }
    if ((value & (1u << 7)) != 0)
    {
        dp_.pipe_busy = 0;
    }
    if ((value & (1u << 8)) != 0)
    {
        dp_.buffer_busy = 0;
    }
    if ((value & (1u << 9)) != 0)
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
    auto read_word = [this, from_dmem](uint32_t address) -> uint32_t
    {
        if (from_dmem)
        {
            return xsflib::LoadBe32(rsp_->Dmem().data() + (address & 0xFF8));
        }

        address &= 0xFFFFF8;
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

        if (op == 0x29)
        {
            RaiseMi(kMiInterruptDp);
        }

        current += length;
    }

    dp_.current = current;
    dp_.status = (dp_.status & ~kDpEndValid) | kDpCommandBufferReady;
}

} // namespace usf::n64
