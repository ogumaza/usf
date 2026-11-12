// SPDX-License-Identifier: MIT

// usfplay's output against the core's, on the sets in FOLDER and their expect.txt files (expect_file.h). For every
// line, of any kind, the test runs usfplay on the track for SECONDS, with no fade, and reads its WAV file. The file
// must hold what usf::Player plays, sample for sample, at its rate: the anchor of that check is the core's playback
// API, played directly and not through the front end.
//
//   usf_play_sets FOLDER USFPLAY WORK
//
// WORK is a folder for the WAV files. The test deletes it at the end.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "xsflib/files.h"

#include "expect_file.h"

namespace
{

namespace fs = std::filesystem;

struct Wav
{
    std::vector<int16_t> samples;
    uint32_t rate = 0;
};

// Runs usfplay on `track`, for `seconds` with no fade, into `out`.
bool RunUsfplay(const fs::path& usfplay, const fs::path& track, const fs::path& out, const std::string& seconds)
{
    const auto quote = [](const fs::path& path)
    {
        return "\"" + xsflib::Utf8(path) + "\"";
    };

    // cmd.exe drops the first and last quotes of a command that starts with a quote. Only Windows therefore gets an
    // outer pair.
    std::string command = quote(usfplay) + " --length " + seconds + " --fade 0 -o " + quote(out) + " " + quote(track);
#ifdef _WIN32
    command = "\"" + command + "\"";
#endif

    return std::system(command.c_str()) == 0;
}

bool CheckTrack(const usf::testing::ExpectLine& line, const fs::path& usfplay, const fs::path& work)
{
    const std::vector<std::string>& fields = line.fields;
    if (!usf::testing::IsExpectLine(fields))
    {
        std::printf("FAIL %s: can't read the line\n", line.text.c_str());
        return false;
    }

    // The renderer, for the line's seconds.
    const std::string& name = fields[0];
    const double seconds = std::atof(fields[2].c_str());
    const fs::path track = line.set / xsflib::PathFromUtf8(name + ".miniusf");
    const fs::path out = work / "render.wav";
    Wav rendered;
    if (!RunUsfplay(usfplay, track, out, fields[2]) ||
        !usf::testing::ReadWavSamples(out, rendered.samples, rendered.rate))
    {
        std::printf("FAIL %s: usfplay gave no WAV file\n", name.c_str());
        return false;
    }

    // Against the core.
    const uint64_t frames = rendered.samples.size() / 2;
    const auto frames_at = [frames](uint32_t)
    {
        return frames;
    };
    Wav core;
    const bool played = usf::testing::PlayOnCore(track, frames_at, core.samples, core.rate);
    if (!played || core.samples.size() != 2 * frames || rendered.rate != core.rate)
    {
        std::printf("FAIL %s: the core played %.3f s at %u Hz, usfplay wrote %.3f s at %u Hz\n", name.c_str(),
                    static_cast<double>(core.samples.size() / 2) / std::max<uint32_t>(core.rate, 1), core.rate,
                    static_cast<double>(frames) / std::max<uint32_t>(rendered.rate, 1), rendered.rate);
        return false;
    }

    const auto differ =
        std::mismatch(rendered.samples.begin(), rendered.samples.end(), core.samples.begin(), core.samples.end());
    if (differ.first != rendered.samples.end() || differ.second != core.samples.end())
    {
        const size_t at = static_cast<size_t>(differ.first - rendered.samples.begin()) / 2;
        std::printf("FAIL %s: usfplay's file differs from the core from %.4f s\n", name.c_str(),
                    static_cast<double>(at) / rendered.rate);
        return false;
    }

    std::printf("pass %s: %.1f s at %u Hz, identical to the core\n", name.c_str(), seconds, rendered.rate);

    return true;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 4)
    {
        std::fprintf(stderr, "usage: usf_play_sets FOLDER USFPLAY WORK\n");
        return 2;
    }

    const fs::path folder = xsflib::PathFromUtf8(argv[1]);
    const fs::path usfplay = xsflib::PathFromUtf8(argv[2]);
    const fs::path work = xsflib::PathFromUtf8(argv[3]);
    std::error_code error;
    fs::create_directories(work, error);

    int failed = 0;
    const std::vector<usf::testing::ExpectLine> lines = usf::testing::ReadExpectLines(folder, failed);
    const int checked = static_cast<int>(lines.size());
    for (const usf::testing::ExpectLine& line : lines)
    {
        failed += CheckTrack(line, usfplay, work) ? 0 : 1;
    }

    fs::remove_all(work, error);
    std::printf("\n%d of %d tracks failed\n", failed, checked);

    return checked > 0 && failed == 0 ? 0 : 1;
}
