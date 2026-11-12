// SPDX-License-Identifier: MIT

#include "n64/pif.h"

#include <algorithm>
#include <stdexcept>

namespace usf::n64
{
namespace
{

constexpr int kCommandByte = static_cast<int>(kPifRamSize) - 1;
constexpr uint8_t kCommandConfigure = 0x01;
constexpr uint8_t kCommandChallenge = 0x02;
constexpr uint8_t kCommandTerminateBoot = 0x08;
constexpr uint8_t kCommandLockRom = 0x10;
constexpr uint8_t kCommandAcquireChecksum = 0x20;
constexpr uint8_t kCommandRunChecksum = 0x40;
constexpr uint8_t kCommandAcknowledge = 0x80;

constexpr int kCartridgeChannel = Pif::kPorts;

// The error flags that the PIF writes into a command's receive-length byte.
constexpr uint8_t kNoDevice = 0x80;
constexpr uint8_t kTimeout = 0x40;

// The joybus commands.
constexpr uint8_t kJoybusInfo = 0x00;
constexpr uint8_t kJoybusState = 0x01;
constexpr uint8_t kJoybusReadAccessory = 0x02;
constexpr uint8_t kJoybusWriteAccessory = 0x03;
constexpr uint8_t kJoybusReadEeprom = 0x04;
constexpr uint8_t kJoybusWriteEeprom = 0x05;
constexpr uint8_t kJoybusResetInfo = 0xFF;

// The info replies: a 2-byte identifier and a status byte. A controller's status 0x02 says that no accessory is
// inserted.
constexpr std::array<uint8_t, 3> kControllerInfo = {0x05, 0x00, 0x02};
constexpr std::array<uint8_t, 3> kEeprom4KbitInfo = {0x00, 0x80, 0x00};
constexpr std::array<uint8_t, 3> kEeprom16KbitInfo = {0x00, 0xC0, 0x00};

constexpr int kAccessoryBlock = 32;
constexpr int kEepromBlock = 8;
constexpr size_t kEeprom4KbitSize = 512;
constexpr size_t kEeprom16KbitSize = 2048;

// The CRC that an accessory sends with its data: CRC-8 with the polynomial 0x85 and a seed of 0.
uint8_t AccessoryCrc(std::span<const uint8_t> data)
{
    uint8_t crc = 0;
    for (const uint8_t byte : data)
    {
        crc ^= byte;
        for (int bit = 0; bit < 8; bit++)
        {
            crc = static_cast<uint8_t>((crc & 0x80) != 0 ? (crc << 1) ^ 0x85 : crc << 1);
        }
    }

    return crc;
}

void CheckPort(int port)
{
    if (port < 0 || port >= Pif::kPorts)
    {
        throw std::out_of_range("a controller port is 0 to 3");
    }
}

} // namespace

void Pif::Reset()
{
    ram_ = {};
    channel_offsets_.fill(-1);
    configured_ = false;
    next_input_ = 0;
}

std::array<uint8_t, kPifRamSize>& Pif::Ram()
{
    return ram_;
}

const std::array<uint8_t, kPifRamSize>& Pif::Ram() const
{
    return ram_;
}

void Pif::ConnectController(int port, bool connected)
{
    CheckPort(port);
    connected_[port] = connected;
}

void Pif::SetController(int port, const ControllerState& state)
{
    CheckPort(port);
    controllers_[port] = state;
}

const ControllerState& Pif::Controller(int port) const
{
    CheckPort(port);

    return controllers_[port];
}

void Pif::AddInput(const InputEvent& event)
{
    CheckPort(event.port);
    script_.push_back(event);
}

void Pif::SetEeprom(EepromType type)
{
    eeprom_type_ = type;
    switch (type)
    {
    case EepromType::kNone:
        eeprom_.clear();
        break;
    case EepromType::k4Kbit:
        eeprom_.assign(kEeprom4KbitSize, 0xFF);
        break;
    case EepromType::k16Kbit:
        eeprom_.assign(kEeprom16KbitSize, 0xFF);
        break;
    }
}

EepromType Pif::Eeprom() const
{
    return eeprom_type_;
}

std::span<uint8_t> Pif::EepromData()
{
    return eeprom_;
}

std::span<const uint8_t> Pif::EepromData() const
{
    return eeprom_;
}

void Pif::OnStateLoaded()
{
    ParseJoybus();
}

void Pif::OnWrite()
{
    uint8_t& command = ram_[kCommandByte];
    if ((command & kCommandConfigure) != 0)
    {
        command &= static_cast<uint8_t>(~kCommandConfigure);
        ParseJoybus();
    }

    // The boot-time commands: the PIF acts on them and clears them. The checksum that IPL2 sends is always accepted.
    command &= static_cast<uint8_t>(~(kCommandTerminateBoot | kCommandLockRom | kCommandRunChecksum));
    if ((command & kCommandAcquireChecksum) != 0)
    {
        command = static_cast<uint8_t>((command & ~kCommandAcquireChecksum) | kCommandAcknowledge);
    }
}

void Pif::OnRead(uint64_t ticks, uint64_t fields)
{
    ApplyInput(ticks, fields);

    if ((ram_[kCommandByte] & kCommandChallenge) != 0)
    {
        RunChallenge();
        return;
    }

    if (configured_)
    {
        RunJoybus();
    }
}

void Pif::ApplyInput(uint64_t ticks, uint64_t fields)
{
    while (next_input_ < script_.size())
    {
        const InputEvent& event = script_[next_input_];
        const uint64_t now = event.clock == InputEvent::Clock::kTicks ? ticks : fields;
        if (event.at > now)
        {
            break;
        }

        controllers_[event.port] = event.state;
        next_input_++;
    }
}

void Pif::ParseJoybus()
{
    // Each channel's command: a byte with the length to send, one with the length to receive, then the bytes. 0x00
    // skips a channel, 0xFD resets it, 0xFE ends the list and 0xFF pads.
    channel_offsets_.fill(-1);
    configured_ = true;
    int offset = 0;
    int channel = 0;
    while (channel < kChannels && offset < kCommandByte)
    {
        const uint8_t send = ram_[offset];
        if (send == 0xFE)
        {
            break;
        }
        if (send == 0xFF)
        {
            offset++;
            continue;
        }
        if (send == 0x00 || send == 0xFD)
        {
            offset++;
            channel++;
            continue;
        }
        if (offset + 1 >= kCommandByte)
        {
            break;
        }

        const int receive = ram_[offset + 1] & 0x3F;
        const int length = 2 + (send & 0x3F) + receive;
        if (offset + length <= kCommandByte)
        {
            channel_offsets_[channel] = offset;
        }

        offset += length;
        channel++;
    }
}

void Pif::RunJoybus()
{
    for (int channel = 0; channel < kChannels; channel++)
    {
        const int offset = channel_offsets_[channel];
        if (offset < 0)
        {
            continue;
        }

        // The top bits of the send length skip the channel (0x80) or reset its device (0x40). Neither writes a reply.
        const uint8_t send = ram_[offset];
        if ((send & 0xC0) != 0)
        {
            continue;
        }

        const int send_length = send & 0x3F;
        const int receive_length = ram_[offset + 1] & 0x3F;

        // A program may have changed the lengths since the last configuration. The channel is skipped when its command
        // and reply no longer fit before the command byte.
        if (offset + 2 + send_length + receive_length > kCommandByte)
        {
            continue;
        }

        const std::span<const uint8_t> command(ram_.data() + offset + 2, static_cast<size_t>(send_length));
        std::array<uint8_t, kPifRamSize> reply{};
        bool present = false;
        int replied = -1;
        if (channel == kCartridgeChannel)
        {
            present = eeprom_type_ != EepromType::kNone;
            if (present)
            {
                replied = EepromCommand(command, reply);
            }
        }
        else
        {
            present = connected_[channel];
            if (present)
            {
                replied = ControllerCommand(channel, command, reply);
            }
        }

        // The PIF writes as many bytes as the device sent, up to the length to receive. A device that sends fewer, or
        // doesn't answer, times out.
        uint8_t flags = 0;
        if (!present)
        {
            flags = kNoDevice;
        }
        else if (replied < receive_length)
        {
            flags = kTimeout;
        }

        const int written = std::clamp(replied, 0, receive_length);
        std::copy_n(reply.begin(), written, ram_.begin() + offset + 2 + send_length);
        ram_[offset + 1] = static_cast<uint8_t>(receive_length | flags);
    }
}

int Pif::ControllerCommand(int port, std::span<const uint8_t> command, std::span<uint8_t> reply)
{
    if (command.empty())
    {
        return -1;
    }

    switch (command[0])
    {
    case kJoybusInfo:
    case kJoybusResetInfo:
        std::copy(kControllerInfo.begin(), kControllerInfo.end(), reply.begin());
        return static_cast<int>(kControllerInfo.size());

    case kJoybusState:
        {
            // While L, R and Start are held, the controller reports its reset bit in place of Start.
            const ControllerState& state = controllers_[port];
            uint16_t buttons = state.buttons & static_cast<uint16_t>(~button::kReset);
            constexpr uint16_t kResetCombination = button::kL | button::kR | button::kStart;
            if ((buttons & kResetCombination) == kResetCombination)
            {
                buttons = static_cast<uint16_t>((buttons & ~button::kStart) | button::kReset);
            }

            reply[0] = static_cast<uint8_t>(buttons >> 8);
            reply[1] = static_cast<uint8_t>(buttons);
            reply[2] = static_cast<uint8_t>(state.stick_x);
            reply[3] = static_cast<uint8_t>(state.stick_y);
            return 4;
        }

    case kJoybusReadAccessory:
        // With no accessory, the data are zeros and the CRC is complemented.
        if (command.size() < 3)
        {
            return -1;
        }
        std::fill_n(reply.begin(), kAccessoryBlock, uint8_t{0});
        reply[kAccessoryBlock] = static_cast<uint8_t>(~AccessoryCrc(reply.first(kAccessoryBlock)));
        return kAccessoryBlock + 1;

    case kJoybusWriteAccessory:
        if (command.size() < 3 + kAccessoryBlock)
        {
            return -1;
        }
        reply[0] = static_cast<uint8_t>(~AccessoryCrc(command.subspan(3, kAccessoryBlock)));
        return 1;

    default:
        return -1;
    }
}

int Pif::EepromCommand(std::span<const uint8_t> command, std::span<uint8_t> reply)
{
    if (command.empty())
    {
        return -1;
    }

    // The 4 Kbit part ignores the top two bits of the block number.
    const size_t blocks = eeprom_.size() / kEepromBlock;
    switch (command[0])
    {
    case kJoybusInfo:
    case kJoybusResetInfo:
        {
            const auto& info = eeprom_type_ == EepromType::k4Kbit ? kEeprom4KbitInfo : kEeprom16KbitInfo;
            std::copy(info.begin(), info.end(), reply.begin());
            return static_cast<int>(info.size());
        }

    case kJoybusReadEeprom:
        {
            if (command.size() < 2)
            {
                return -1;
            }

            const size_t start = (command[1] % blocks) * kEepromBlock;
            std::copy_n(eeprom_.begin() + static_cast<std::ptrdiff_t>(start), kEepromBlock, reply.begin());
            return kEepromBlock;
        }

    case kJoybusWriteEeprom:
        {
            if (command.size() < 2 + kEepromBlock)
            {
                return -1;
            }

            const size_t start = (command[1] % blocks) * kEepromBlock;
            std::copy_n(command.begin() + 2, kEepromBlock, eeprom_.begin() + static_cast<std::ptrdiff_t>(start));
            reply[0] = 0x00; // The write started.
            return 1;
        }

    default:
        return -1;
    }
}

void Pif::RunChallenge()
{
    // The CIC-NUS-6105 challenge isn't answered. The command bit is cleared so that the program doesn't wait.
    ram_[kCommandByte] &= static_cast<uint8_t>(~kCommandChallenge);
}

} // namespace usf::n64
