// SPDX-License-Identifier: MIT

// Runs a test ROM and reports its verdict. The ROMs aren't in the repo (tests/test_roms.cmake).
//
//   usf_test_rom systemtest ROM [SECONDS]
//       n64-systemtest. It writes its results to the debug port and ends with a summary. The test passes when no test
//       failed outside the categories that the core doesn't cover: the RDP, the caches and reverse-endian user mode.
//
//   usf_test_rom peterlemon ROM [SECONDS]
//       One of PeterLemon's N64 tests. It draws its results: "PASS" in its green font, "FAIL" in its red one. The
//       fonts are the files FontGreen8x8.bin and FontRed8x8.bin beside the ROM. The test passes when the frame shows
//       at least one PASS and no FAIL. With the environment variable USF_FRAME set to a path, it also writes the
//       frame there as a PPM image.
//
//   usf_test_rom peterlemon-frame ROM SHA1 LINES
//       One of PeterLemon's tests that draws values but no verdict. The test passes when the SHA-1 of the frame's
//       first LINES lines, as RGB bytes, is SHA1. The hashes come from the screenshots in the tests' repository.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "xsflib/files.h"
#include "xsflib/sha1.h"

#include "n64/system.h"

namespace
{

using usf::n64::System;

constexpr const char* kUsage =
    "usage: usf_test_rom systemtest|peterlemon ROM [SECONDS]\n"
    "       usf_test_rom peterlemon-frame ROM SHA1 LINES\n";

// The width of a frame when the VI reports none.
constexpr uint32_t kDefaultFrameWidth = 640;

bool LoadRom(System& system, const std::string& path)
{
    std::vector<uint8_t> rom;
    if (!xsflib::ReadFile(xsflib::PathFromUtf8(path), rom))
    {
        std::fprintf(stderr, "error: can't read %s\n", path.c_str());
        return false;
    }

    system.LoadRom(std::move(rom));

    return true;
}

// With the environment variable USF_TRACE set, prints the first 64 exceptions other than interrupts. The tests cause
// exceptions on purpose, but when a test goes wrong, the last ones show where.
void TraceExceptions(System& system)
{
    if (std::getenv("USF_TRACE") == nullptr)
    {
        return;
    }

    auto count = std::make_shared<int>(0);
    system.GetCpu().SetExceptionHook(
        [&system, count](usf::n64::ExceptionCode code, uint64_t pc)
        {
            if (code != usf::n64::ExceptionCode::kInterrupt && ++*count <= 64)
            {
                std::fprintf(stderr, "exception %u at 0x%08llX, Status 0x%08llX\n", static_cast<unsigned>(code),
                             static_cast<unsigned long long>(pc & 0xFFFFFFFF),
                             static_cast<unsigned long long>(system.GetCpu().Cop0(usf::n64::kCop0Status)));
            }
        });
}

// Runs `system` in steps until the summary appears in `text`, for up to `seconds`. Returns false after printing that it
// didn't.
bool RunUntilSummary(System& system, const std::string& text, double seconds)
{
    constexpr double kStep = 0.5;
    for (double t = 0; t < seconds; t += kStep)
    {
        system.RunSeconds(kStep);
        if (text.find("Finished in") != std::string::npos)
        {
            break;
        }
    }

    if (text.find("Finished in") == std::string::npos)
    {
        std::printf("\nno summary after %.1f s (PC 0x%08llX)\n", seconds,
                    static_cast<unsigned long long>(system.GetCpu().Pc() & 0xFFFFFFFF));
        return false;
    }

    return true;
}

// The names of the tests that failed in `text`, outside the categories that the core doesn't cover.
std::set<std::string> FailuresInScope(const std::string& text)
{
    // Each failure starts "Test '<name>' failed". The RDP's tests can't pass, because the RDP doesn't draw, and the
    // cache tests can't either, because the core has no caches. The core doesn't reverse the byte order in user mode
    // when Status.RE is set. Games run in kernel mode, so none depends on it.
    static constexpr std::array<std::string_view, 5> kOutOfScope = {"RDP", "CACHE", "ICACHE", "DCACHE", "RE "};
    std::set<std::string> failed;
    size_t start = 0;
    while ((start = text.find("Test '", start)) != std::string::npos)
    {
        start += 6;
        const size_t end = text.find("' ", start);
        if (end == std::string::npos)
        {
            break;
        }

        const std::string name = text.substr(start, end - start);
        if (text.compare(end, 9, "' failed:") != 0 && text.compare(end, 7, "' with ") != 0)
        {
            continue;
        }

        const auto out_of_scope = [&name](std::string_view prefix)
        {
            return name.starts_with(prefix);
        };
        if (!std::any_of(kOutOfScope.begin(), kOutOfScope.end(), out_of_scope))
        {
            failed.insert(name);
        }
    }

    return failed;
}

int RunSystemTest(const std::string& path, double seconds)
{
    usf::n64::Options options;
    options.is_viewer = true;
    options.skip_graphics_tasks = false;
    options.rdram_size = usf::n64::kRdramSize8;
    System system(options);
    if (!LoadRom(system, path))
    {
        return 1;
    }

    std::string text;
    system.SetDebugOutput(
        [&](std::string_view chunk)
        {
            text.append(chunk);
            std::fwrite(chunk.data(), 1, chunk.size(), stdout);
            std::fflush(stdout);
        });

    const usf::n64::BootInfo boot = system.Boot();
    if (!boot.booted)
    {
        std::printf("boot failed: %s\n", boot.problem.c_str());
        return 1;
    }

    TraceExceptions(system);
    if (!RunUntilSummary(system, text, seconds))
    {
        return 1;
    }

    const std::set<std::string> failed = FailuresInScope(text);
    for (const std::string& name : failed)
    {
        std::printf("failed: %s\n", name.c_str());
    }
    std::printf("\ntests that failed outside the categories not covered: %zu\n", failed.size());

    return failed.empty() ? 0 : 1;
}

// The lines of a frame of `width` pixels: a 4:3 picture.
uint32_t FrameHeight(uint32_t width)
{
    return width * 3 / 4;
}

// Whether `font` holds the glyph of each character of `text`: 8 rows of 8 pixels of 4 bytes, 256 bytes for each
// character code.
bool CoversText(const std::vector<uint8_t>& font, std::string_view text)
{
    const auto covered = [&font](char c)
    {
        return static_cast<uint8_t>(c) * 256u + 256 <= font.size();
    };

    return std::all_of(text.begin(), text.end(), covered);
}

// Whether `text`, drawn in `font`, starts at byte `pixel` of `rdram`, in a frame whose lines are `stride` bytes apart.
bool TextAt(std::span<const uint8_t> rdram, const std::vector<uint8_t>& font, std::string_view text, size_t pixel,
            uint32_t stride)
{
    for (size_t c = 0; c < text.size(); c++)
    {
        const size_t glyph = static_cast<uint8_t>(text[c]) * 256u;
        for (uint32_t row = 0; row < 8; row++)
        {
            const size_t start = pixel + size_t{row} * stride + c * 32;
            if (start + 32 > rdram.size())
            {
                return false;
            }

            const std::span<const uint8_t> line = rdram.subspan(start, 32);
            if (!std::equal(line.begin(), line.end(), font.begin() + static_cast<std::ptrdiff_t>(glyph + row * 32)))
            {
                return false;
            }
        }
    }

    return true;
}

// Counts where `text`, drawn in `font`, appears in the 32-bit frame of `width` pixels at `origin`. With the environment
// variable USF_TRACE set, prints where each one is. Returns 0 when `font` doesn't cover `text`.
int CountText(const System& system, uint32_t origin, uint32_t width, const std::vector<uint8_t>& font,
              std::string_view text)
{
    if (!CoversText(font, text))
    {
        return 0;
    }

    const uint32_t height = FrameHeight(width);
    const bool trace = std::getenv("USF_TRACE") != nullptr;
    const auto rdram = system.Rdram();
    const uint32_t stride = width * 4;
    int count = 0;
    for (uint32_t y = 0; y + 8 <= height; y++)
    {
        for (uint32_t x = 0; x + 8 * text.size() <= width; x++)
        {
            if (!TextAt(rdram, font, text, size_t{origin} + size_t{y} * stride + size_t{x} * 4, stride))
            {
                continue;
            }

            count++;
            if (trace)
            {
                std::printf("%.*s at (%u, %u)\n", static_cast<int>(text.size()), text.data(), x, y);
            }
        }
    }

    return count;
}

// The RGB bytes of the first `lines` lines of the 32-bit frame of `width` pixels at `origin`. A pixel past the end of
// RDRAM is black.
std::vector<uint8_t> FrameRgb(const System& system, uint32_t origin, uint32_t width, uint32_t lines)
{
    const auto rdram = system.Rdram();
    std::vector<uint8_t> rgb;
    for (uint32_t i = 0; i < width * lines; i++)
    {
        const size_t pixel = size_t{origin} + size_t{i} * 4;
        for (size_t c = 0; c < 3; c++)
        {
            rgb.push_back(pixel + c < rdram.size() ? rdram[pixel + c] : uint8_t{0});
        }
    }

    return rgb;
}

// Writes the 32-bit frame of `width` pixels at `origin` to `path` as a PPM image, for comparison with the screenshots
// that come with some of the tests.
void WriteFrame(const System& system, uint32_t origin, uint32_t width, const char* path)
{
    const uint32_t height = FrameHeight(width);
    std::string header = "P6\n" + std::to_string(width) + " " + std::to_string(height) + "\n255\n";
    std::vector<uint8_t> image(header.begin(), header.end());
    const std::vector<uint8_t> rgb = FrameRgb(system, origin, width, height);
    image.insert(image.end(), rgb.begin(), rgb.end());

    if (auto error = xsflib::WriteFile(xsflib::PathFromUtf8(path), image))
    {
        std::fprintf(stderr, "error: %s\n", error->c_str());
    }
}

// Boots one of PeterLemon's tests and runs it for `seconds`.
bool RunPeterLemonRom(System& system, const std::string& path, double seconds)
{
    if (!LoadRom(system, path))
    {
        return false;
    }

    const usf::n64::BootInfo boot = system.Boot();
    if (!boot.booted)
    {
        std::printf("boot failed: %s\n", boot.problem.c_str());
        return false;
    }

    // The tests' expected values were recorded on a console that started them from a flash cartridge. Its menu leaves
    // COP0 Status at 0x241000E0, not the 0x34000000 that a cold boot leaves. The exception tests check that value, and
    // the vector that its KX bit selects. n64-systemtest's startup test describes the same state.
    system.GetCpu().SetCop0(usf::n64::kCop0Status, 0x241000E0);

    TraceExceptions(system);
    system.RunSeconds(seconds);

    return true;
}

usf::n64::Options PeterLemonOptions()
{
    usf::n64::Options options;
    options.skip_graphics_tasks = false;
    options.rdram_size = usf::n64::kRdramSize8;

    return options;
}

int RunPeterLemonFrame(const std::string& path, const std::string& expected, uint32_t lines)
{
    System system(PeterLemonOptions());
    if (!RunPeterLemonRom(system, path, 4))
    {
        return 1;
    }

    const System::Frame frame = system.VideoFrame();
    const uint32_t width = frame.width == 0 ? kDefaultFrameWidth : frame.width;
    xsflib::Sha1 sha1;
    sha1.Update(FrameRgb(system, frame.origin, width, lines));

    const std::string actual = xsflib::Sha1::Hex(sha1.Finish());
    std::printf("%s: frame %s, expected %s\n", path.c_str(), actual.c_str(), expected.c_str());

    return actual == expected ? 0 : 1;
}

int RunPeterLemon(const std::string& path, double seconds)
{
    const std::filesystem::path folder = xsflib::PathFromUtf8(path).parent_path();
    std::vector<uint8_t> green;
    std::vector<uint8_t> red;
    if (!xsflib::ReadFile(folder / "FontGreen8x8.bin", green) || !xsflib::ReadFile(folder / "FontRed8x8.bin", red))
    {
        std::fprintf(stderr, "error: the fonts aren't beside %s\n", path.c_str());
        return 1;
    }

    if (!CoversText(green, "PASS") || !CoversText(red, "FAIL"))
    {
        std::fprintf(stderr, "error: a font beside %s is too short for PASS or FAIL\n", path.c_str());
        return 1;
    }

    System system(PeterLemonOptions());
    if (!RunPeterLemonRom(system, path, seconds))
    {
        return 1;
    }

    const System::Frame frame = system.VideoFrame();
    const uint32_t width = frame.width == 0 ? kDefaultFrameWidth : frame.width;
    if (const char* dump = std::getenv("USF_FRAME"))
    {
        WriteFrame(system, frame.origin, width, dump);
    }

    const int passes = CountText(system, frame.origin, width, green, "PASS");
    const int fails = CountText(system, frame.origin, width, red, "FAIL");
    std::printf("%s: %d PASS, %d FAIL (frame at 0x%06X, %u pixels wide)\n", path.c_str(), passes, fails, frame.origin,
                width);

    return passes > 0 && fails == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        std::fprintf(stderr, "%s", kUsage);
        return 2;
    }

    const std::string mode = argv[1];
    const std::string path = argv[2];
    if (mode == "systemtest")
    {
        return RunSystemTest(path, argc > 3 ? std::atof(argv[3]) : 600);
    }
    if (mode == "peterlemon")
    {
        return RunPeterLemon(path, argc > 3 ? std::atof(argv[3]) : 4);
    }
    if (mode == "peterlemon-frame")
    {
        if (argc < 5)
        {
            std::fprintf(stderr, "%s", kUsage);
            return 2;
        }

        return RunPeterLemonFrame(path, argv[3], static_cast<uint32_t>(std::atoi(argv[4])));
    }

    std::fprintf(stderr, "error: unknown mode %s\n", mode.c_str());

    return 2;
}
