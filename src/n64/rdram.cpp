// SPDX-License-Identifier: MIT

// The RDRAM chips: the registers that IPL3 sets up, the accesses that go through the chips until RDRAM becomes plain
// memory, and MI_MODE's repeat mode.

#include <array>

#include "xsflib/bytes.h"

#include "n64/bits.h"
#include "n64/system.h"

namespace usf::n64
{
namespace
{

// The parts of an address in the RDRAM chips' register space: the broadcast bit, the chip select, the offset in the
// chip and the index of the register at that offset.
struct RdramRegisterAddress
{
    bool broadcast = false;
    uint32_t select = 0;
    uint32_t offset = 0;
    uint32_t index = 0;
};

RdramRegisterAddress DecodeRdramRegister(uint32_t paddr)
{
    const uint32_t offset = paddr & 0x3FF;
    return {((paddr >> 19) & 1) != 0, (paddr >> 10) & 0x1FF, offset, (offset >> 2) & 15};
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
// kRdramCurrentLow or less. Between the two, each set bit survives with a probability that rises with the current. IPL3
// finds the current it sets by watching the bits. The two values lie within the ranges that chips show.
constexpr uint32_t kRdramCurrentLow = 9;
constexpr uint32_t kRdramCurrentHigh = 15;
constexpr uint32_t kRdramChipSize = 0x200000;

// A chip's offsets from this one up reach its ROW register.
constexpr uint32_t kRdramRowOffset = 0x200;

uint32_t DecodeDeviceId(uint32_t value)
{
    return ((value >> 26) & 0x3F) | (((value >> 23) & 1) << 6) | (((value >> 8) & 0xFF) << 7) |
           (((value >> 7) & 1) << 15);
}

} // namespace

uint32_t System::MiRepeatLength() const
{
    return (mi_.mode & kMiModeRepeatLength) + 1;
}

// The RDRAM chips' registers. Bit 19 of the address broadcasts a write to every chip. Bits 10 to 18 select a chip by
// its device ID. A chip that isn't enabled yet answers only when it's the first such chip, which lets IPL3 give each
// chip its ID in turn.

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

    // Until the DELAY register sets a write delay of one, a write takes effect only when the MI repeats it (init mode
    // with a length of 16 bytes or more), and its halfwords arrive swapped.
    if (chip.write_delay != 1)
    {
        const bool init_mode = (mi_.mode & kMiModeRepeat) != 0;
        const uint32_t repeat = init_mode ? MiRepeatLength() : 0;
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
    const RdramRegisterAddress address = DecodeRdramRegister(paddr);
    if (address.broadcast)
    {
        return 0;
    }

    RdramChip* chip = SelectRdramChip(address.select);
    if (chip == nullptr)
    {
        return 0;
    }
    if (address.offset >= kRdramRowOffset)
    {
        return chip->row;
    }

    return ReadRdramChipRegister(*chip, address.index);
}

void System::WriteRdramRegister(uint32_t paddr, uint32_t value)
{
    const RdramRegisterAddress address = DecodeRdramRegister(paddr);
    if (address.broadcast)
    {
        for (RdramChip& chip : rdram_chips_)
        {
            if (!chip.present)
            {
                continue;
            }

            if (address.offset >= kRdramRowOffset)
            {
                chip.row = value;
            }
            else
            {
                WriteRdramChipRegister(chip, address.index, value);
            }
        }

        // Repeat mode lasts for one write to the RDRAM's bus.
        mi_.mode &= ~kMiModeRepeat;
        UpdateRdramMapping();
        return;
    }

    RdramChip* chip = SelectRdramChip(address.select);
    if (chip == nullptr)
    {
        return;
    }
    if (address.offset >= kRdramRowOffset)
    {
        chip->row = value;
        return;
    }

    WriteRdramChipRegister(*chip, address.index, value);
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

const System::RdramChip* System::MappedRdramChip(uint32_t paddr, uint32_t& offset) const
{
    for (size_t n = 0; n < rdram_chips_.size(); n++)
    {
        const RdramChip& chip = rdram_chips_[n];
        if (chip.present && chip.enable && (chip.device_id >> 1) == (paddr >> 21))
        {
            offset = static_cast<uint32_t>(n) * kRdramChipSize + (paddr & (kRdramChipSize - 1));
            return &chip;
        }
    }

    return nullptr;
}

uint32_t System::ReadMappedRdram32(uint32_t paddr)
{
    uint32_t offset = 0;
    const RdramChip* chip = MappedRdramChip(paddr, offset);
    if (chip == nullptr || offset + 4 > rdram_.size())
    {
        return 0;
    }

    if (Coverage* coverage = ActiveCoverage())
    {
        coverage->ReadRdram(offset, 4);
    }

    const uint32_t value = xsflib::LoadBe32(rdram_.data() + offset);
    if (chip->current >= kRdramCurrentHigh)
    {
        return value;
    }
    if (chip->current <= kRdramCurrentLow)
    {
        return 0;
    }

    // A fixed xorshift sequence decides which bits survive, so that every run gives the same result.
    const uint32_t chance = (chip->current - kRdramCurrentLow) * 256 / (kRdramCurrentHigh - kRdramCurrentLow);
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
        const uint32_t word = BusWord(paddr, value, size);
        xsflib::StoreBe32(pattern.data(), word);
        xsflib::StoreBe32(pattern.data() + 4, word);
    }

    mi_.mode &= ~kMiModeRepeat;
    const uint32_t length = MiRepeatLength();
    const uint32_t base = paddr & ~7u;
    const uint32_t page = paddr & ~(kRdramPageSize - 1);
    for (uint32_t i = paddr - base; i < length; i++)
    {
        const uint32_t address = page | ((base + i) & (kRdramPageSize - 1));
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
    uint32_t offset = 0;
    if (MappedRdramChip(paddr, offset) == nullptr || offset + size > rdram_.size())
    {
        return;
    }

    StoreBe(rdram_.data() + offset, value, size);
    if (Coverage* coverage = ActiveCoverage())
    {
        coverage->WriteRdram(offset, static_cast<uint32_t>(size));
    }
}

} // namespace usf::n64
