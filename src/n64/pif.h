// SPDX-License-Identifier: MIT

// The PIF: its 64 bytes of RAM, the joybus commands that the CPU leaves there, and the commands of its command byte.
// The PIF ROM isn't emulated. System::Boot (boot.cpp) leaves the state that the ROM's code would leave.
//
// By default nothing is connected, as in the existing USF players. Every joybus command then comes back with the "no
// device" bit (0x80) in its receive-length byte. A standard controller can be connected to each of the four ports,
// and an EEPROM of 4 or 16 Kbit to the cartridge's channel. The replies follow n64brew's pages on the joybus protocol
// and the PIF.

#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace usf::n64
{

// The buttons of a standard controller, as the first two bytes of its reply to the state command (0x01) hold them.
namespace button
{

inline constexpr uint16_t kA = 0x8000;
inline constexpr uint16_t kB = 0x4000;
inline constexpr uint16_t kZ = 0x2000;
inline constexpr uint16_t kStart = 0x1000;
inline constexpr uint16_t kUp = 0x0800;
inline constexpr uint16_t kDown = 0x0400;
inline constexpr uint16_t kLeft = 0x0200;
inline constexpr uint16_t kRight = 0x0100;
inline constexpr uint16_t kReset = 0x0080; // Set by the controller itself while L, R and Start are held.
inline constexpr uint16_t kL = 0x0020;
inline constexpr uint16_t kR = 0x0010;
inline constexpr uint16_t kCUp = 0x0008;
inline constexpr uint16_t kCDown = 0x0004;
inline constexpr uint16_t kCLeft = 0x0002;
inline constexpr uint16_t kCRight = 0x0001;

} // namespace button

// What a standard controller's user holds: the buttons, and the stick's position as two's-complement bytes.
struct ControllerState
{
    uint16_t buttons = 0;
    int8_t stick_x = 0;
    int8_t stick_y = 0;
};

// A scripted change of one controller's state, at a time in ticks of the CPU clock or in video fields since reset.
struct InputEvent
{
    enum class Clock
    {
        kTicks,
        kFields,
    };

    Clock clock = Clock::kFields;
    uint64_t at = 0;
    int port = 0;
    ControllerState state;
};

enum class EepromType
{
    kNone,
    k4Kbit,
    k16Kbit,
};

class Pif
{
public:
    static constexpr int kPorts = 4;

    // Clears the RAM and the joybus configuration. The connected devices, their states, the EEPROM's contents and the
    // input script stay. The script starts again from its first event.
    void Reset();

    std::array<uint8_t, 64>& Ram();
    const std::array<uint8_t, 64>& Ram() const;

    // Called after a save state has restored the RAM. A state holds no joybus configuration, so the frame in the RAM
    // becomes the last configuration. libultra sets the command byte's configure bit only when it rebuilds a frame.
    void OnStateLoaded();

    // Called after the CPU or the SI writes PIF RAM. Acts on the command byte.
    void OnWrite();

    // Called before the SI reads PIF RAM into RDRAM, with the time since reset. Applies the input script up to that
    // time, then runs the joybus commands that the last configuration set up.
    void OnRead(uint64_t ticks, uint64_t fields);

    // Controllers. A port is 0 to 3. A controller holds no accessory.
    void ConnectController(int port, bool connected);
    bool ControllerConnected(int port) const;
    void SetController(int port, const ControllerState& state);
    const ControllerState& Controller(int port) const;

    // Adds an event to the input script. Events take effect in the order they were added, each at the first joybus
    // run at or after its time. An event never takes effect before the one added ahead of it.
    void AddInput(const InputEvent& event);
    void ClearInput();

    // The EEPROM on the cartridge's channel. Setting the type clears the contents to 0xFF. A write takes effect at
    // once, and the EEPROM never reports itself busy.
    void SetEeprom(EepromType type);
    EepromType Eeprom() const;
    std::span<uint8_t> EepromData();
    std::span<const uint8_t> EepromData() const;

private:
    void ParseJoybus();
    void RunJoybus();
    void RunChallenge();
    void ApplyInput(uint64_t ticks, uint64_t fields);

    // Runs one command on a channel. Returns the number of reply bytes written to `reply`, or -1 when no device
    // answers.
    int ControllerCommand(int port, std::span<const uint8_t> command, std::span<uint8_t> reply);
    int EepromCommand(std::span<const uint8_t> command, std::span<uint8_t> reply);

    std::array<uint8_t, 64> ram_{};
    std::array<int, 5> channel_offsets_{}; // Where each channel's command starts, or -1.
    bool configured_ = false;

    std::array<bool, kPorts> connected_{};
    std::array<ControllerState, kPorts> controllers_{};
    std::vector<InputEvent> script_;
    size_t next_input_ = 0;

    EepromType eeprom_type_ = EepromType::kNone;
    std::vector<uint8_t> eeprom_;
};

} // namespace usf::n64
