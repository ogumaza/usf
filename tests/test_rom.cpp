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

#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "xsflib/bytes.h"
#include "xsflib/files.h"
#include "xsflib/sha1.h"

#include "n64/system.h"

namespace
{

using usf::n64::System;

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

    // Run until the summary appears.
    const double step = 0.5;
    for (double t = 0; t < seconds; t += step)
    {
        system.RunSeconds(step);
        if (text.find("Finished in") != std::string::npos)
        {
            break;
        }
    }

    if (text.find("Finished in") == std::string::npos)
    {
        std::printf("\nno summary after %.1f s (PC 0x%08llX)\n", seconds,
                    static_cast<unsigned long long>(system.GetCpu().Pc() & 0xFFFFFFFF));
        return 1;
    }

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

        bool covered = true;
        for (std::string_view prefix : kOutOfScope)
        {
            if (std::string_view(name).substr(0, prefix.size()) == prefix)
            {
                covered = false;
            }
        }
        if (covered)
        {
            failed.insert(name);
        }
    }

    for (const std::string& name : failed)
    {
        std::printf("failed: %s\n", name.c_str());
    }
    std::printf("\ntests that failed outside the categories not covered: %zu\n", failed.size());

    return failed.empty() ? 0 : 1;
}

// Counts where `text`, drawn in `font`, appears in the 32-bit frame of `width` pixels at `origin`. With the
// environment variable USF_TRACE set, prints where each one is.
int CountText(const System& system, uint32_t origin, uint32_t width, const std::vector<uint8_t>& font, const char* text)
{
    const bool trace = std::getenv("USF_TRACE") != nullptr;
    const auto rdram = system.Rdram();
    const size_t length = std::char_traits<char>::length(text);
    const uint32_t stride = width * 4;
    int count = 0;
    for (uint32_t y = 0; y + 8 <= 480; y++)
    {
        for (uint32_t x = 0; x + 8 * length <= width; x++)
        {
            bool match = true;
            for (size_t c = 0; c < length && match; c++)
            {
                const size_t glyph = static_cast<uint8_t>(text[c]) * 256u;
                if (glyph + 256 > font.size())
                {
                    return 0;
                }

                for (uint32_t row = 0; row < 8 && match; row++)
                {
                    const size_t pixel = size_t{origin} + size_t{y + row} * stride + (x + 8 * c) * 4;
                    if (pixel + 32 > rdram.size())
                    {
                        match = false;
                        break;
                    }

                    for (uint32_t i = 0; i < 32; i++)
                    {
                        if (rdram[pixel + i] != font[glyph + row * 32 + i])
                        {
                            match = false;
                            break;
                        }
                    }
                }
            }
            if (match)
            {
                count++;
                if (trace)
                {
                    std::printf("%s at (%u, %u)\n", text, x, y);
                }
            }
        }
    }

    return count;
}

// Writes the 32-bit frame of `width` pixels at `origin` to `path` as a PPM image, for comparison with the screenshots
// that come with some of the tests.
void WriteFrame(const System& system, uint32_t origin, uint32_t width, const char* path)
{
    const auto rdram = system.Rdram();
    const uint32_t height = width * 3 / 4;
    std::string header = "P6\n" + std::to_string(width) + " " + std::to_string(height) + "\n255\n";
    std::vector<uint8_t> image(header.begin(), header.end());
    for (uint32_t i = 0; i < width * height; i++)
    {
        const size_t pixel = size_t{origin} + size_t{i} * 4;
        for (size_t c = 0; c < 3; c++)
        {
            image.push_back(pixel + c < rdram.size() ? rdram[pixel + c] : 0);
        }
    }

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
    // COP0 Status at 0x241000E0, not the 0x34000000 that a cold boot leaves. The exception tests check that value,
    // and the vector that its KX bit selects. n64-systemtest's startup test describes the same state.
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
    const uint32_t width = frame.width == 0 ? 640 : frame.width;
    const auto rdram = system.Rdram();
    xsflib::Sha1 sha1;
    for (uint32_t i = 0; i < width * lines; i++)
    {
        const size_t pixel = size_t{frame.origin} + size_t{i} * 4;
        const std::array<uint8_t, 3> rgb = {
            pixel < rdram.size() ? rdram[pixel] : uint8_t{0},
            pixel + 1 < rdram.size() ? rdram[pixel + 1] : uint8_t{0},
            pixel + 2 < rdram.size() ? rdram[pixel + 2] : uint8_t{0},
        };
        sha1.Update(rgb);
    }

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

    System system(PeterLemonOptions());
    if (!RunPeterLemonRom(system, path, seconds))
    {
        return 1;
    }

    const System::Frame frame = system.VideoFrame();
    const uint32_t width = frame.width == 0 ? 640 : frame.width;
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
        std::fprintf(stderr,
                     "usage: usf_test_rom systemtest|peterlemon ROM [SECONDS]\n"
                     "       usf_test_rom peterlemon-frame ROM SHA1 LINES\n");
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
    if (mode == "peterlemon-frame" && argc > 4)
    {
        return RunPeterLemonFrame(path, argv[3], static_cast<uint32_t>(std::atoi(argv[4])));
    }

    std::fprintf(stderr, "error: unknown mode %s\n", mode.c_str());

    return 2;
}
