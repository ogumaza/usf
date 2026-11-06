// SPDX-License-Identifier: MIT

// Writes a USF set made from small programs (test_set.h) into a folder, for the tests of the front ends in their hosts.
// The set contains no game data. The tests play it in place of a rip.
//
// "01 Heavy" gives the emulator a heavy load: more instructions per second of output, on the CPU and on the RSP, than
// the music of real games gives. A set lighter than real rips would hide a stutter. A much heavier one would give false
// warnings. Time the track again when the core's speed changes. "02 Light" is a track with little load, at another
// rate.
//
//   usf_make_test_set FOLDER

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "xsflib/files.h"
#include "xsflib/psf/psf.h"

#include "test_set.h"

namespace
{

namespace fs = std::filesystem;
using usf::testing::kCpuInstructionsPerBuffer;
using usf::testing::kCpuInstructionsPerLoop;
using usf::testing::kRspInstructionsPerLoop;
using usf::testing::kTestBufferFrames;
using usf::testing::TestRate;
using usf::testing::TestTrack;

// The loads, in instructions per second of output.
constexpr uint64_t kHeavyCpuPerSecond = 8800000;
constexpr uint64_t kHeavyRspPerSecond = 18400000;

// The two tracks' DACRATEs: 32,006 Hz and 22,047 Hz.
constexpr uint32_t kHeavyDacRate = 1520;
constexpr uint32_t kLightDacRate = 2207;

// The ramp's slope and the samples' bits: a quiet sawtooth of a few hundred hertz, should anyone play the files.
constexpr uint32_t kStep = 37;
constexpr uint32_t kMask = 0x0FFF;

TestTrack HeavyTrack()
{
    const uint64_t buffers_per_second = TestRate(kHeavyDacRate) / kTestBufferFrames;
    TestTrack track;
    track.dac_rate = kHeavyDacRate;
    track.step = kStep;
    track.mask = kMask;
    track.cpu_loops = static_cast<uint32_t>((kHeavyCpuPerSecond / buffers_per_second - kCpuInstructionsPerBuffer) /
                                            kCpuInstructionsPerLoop);
    track.rsp_loops = static_cast<uint32_t>(kHeavyRspPerSecond / buffers_per_second / kRspInstructionsPerLoop);

    return track;
}

TestTrack LightTrack()
{
    TestTrack track;
    track.dac_rate = kLightDacRate;
    track.step = kStep;
    track.mask = kMask;
    track.cpu_loops = 1000;
    track.rsp_loops = 1000;

    return track;
}

xsflib::psf::Tags TrackTags(const std::string& title)
{
    return {
        {"_lib", "test.usflib"}, {"_enablecompare", "1"},  {"_enablefifofull", "1"},
        {"title", title},        {"game", "usf test set"}, {"length", "32"},
        {"fade", "2"},
    };
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        std::fprintf(stderr, "usage: usf_make_test_set FOLDER\n");
        return 2;
    }

    const fs::path folder = xsflib::PathFromUtf8(argv[1]);
    std::error_code error;
    fs::create_directories(folder, error);

    const auto write = [&folder](const char* name, const std::vector<uint8_t>& bytes)
    {
        if (const auto problem = xsflib::WriteFile(folder / name, bytes))
        {
            std::fprintf(stderr, "error: %s\n", problem->c_str());
            return false;
        }

        return true;
    };

    const bool written =
        write("test.usflib", usf::testing::MakeTestLibrary({{"_enablecompare", "1"}, {"_enablefifofull", "1"}})) &&
        write("01 Heavy.miniusf", usf::testing::MakeTestMini(HeavyTrack(), TrackTags("Heavy"))) &&
        write("02 Light.miniusf", usf::testing::MakeTestMini(LightTrack(), TrackTags("Light")));

    // The check of the renderer's output against the core's (play_sets.cpp).
    const std::string expect = "01 Heavy\tcore\t3\n02 Light\tcore\t3\n";

    return written && write("expect.txt", std::vector<uint8_t>(expect.begin(), expect.end())) ? 0 : 1;
}
