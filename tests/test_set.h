// SPDX-License-Identifier: MIT

// A USF set made from small programs, with no game data, for tests of the player's front ends. The library holds a MIPS
// program and an RSP program. Each mini writes a block of parameters into RDRAM. A real set selects a track with a word
// of RDRAM in the same way.
//
// The CPU's program fills buffers of kTestBufferFrames frames with a ramp and hands them to the audio interface. For
// each buffer it also runs a work loop of integer, memory and floating-point instructions, and an RSP task of vector
// instructions. Whenever the AI is full, it sleeps in an idle loop until the AI's interrupt. A game's idle thread does
// the same. A track's parameters therefore set its rate and its load on the emulator.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "xsflib/psf/psf.h"

namespace usf::testing
{

inline constexpr uint32_t kTestBufferFrames = 1024;

// The instructions of one pass of each work loop, for working out a load.
inline constexpr uint32_t kCpuInstructionsPerLoop = 11;
inline constexpr uint32_t kRspInstructionsPerLoop = 10;

// The CPU instructions that a buffer takes without its work loop: 12 for each frame of the ramp, and the rest.
inline constexpr uint32_t kCpuInstructionsPerBuffer = 12 * kTestBufferFrames + 60;

// What a mini asks of the programs.
struct TestTrack
{
    uint32_t dac_rate = 1520;  // AI_DACRATE. The default gives 32,006 Hz on NTSC.
    uint32_t cpu_loops = 0;    // Passes of the CPU's work loop for each buffer.
    uint32_t rsp_loops = 0;    // Passes of the RSP program's loop for each buffer. With 0, no task starts.
    uint32_t step = 1;         // How much the ramp rises from frame to frame.
    uint32_t mask = 0xFFFF;    // The bits of the ramp that a sample holds.
    uint32_t change_after = 0; // The buffers before the rate changes. With 0, it never changes.
    uint32_t second_dac_rate = 0;

    // The program stops sending buffers after this many buffers less one, and goes on running. With 0, it never stops.
    // With 1, it sends none at all.
    uint32_t stop_after = 0;
};

// The rate of a DACRATE on NTSC: the video clock over DACRATE + 1.
uint32_t TestRate(uint32_t dac_rate);

// The samples of frame `frame` of the stream that a track's program sends.
int16_t TestLeft(const TestTrack& track, uint64_t frame);
int16_t TestRight(const TestTrack& track, uint64_t frame);

// The library's file: the ROM and the state, with `tags`.
std::vector<uint8_t> MakeTestLibrary(const xsflib::psf::Tags& tags);

// A mini's file. `tags` should name the library in `_lib`.
std::vector<uint8_t> MakeTestMini(const TestTrack& track, const xsflib::psf::Tags& tags);

} // namespace usf::testing
