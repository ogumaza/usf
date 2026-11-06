// SPDX-License-Identifier: MIT

// n64run: boots a cartridge, or loads a USF track with its libraries, runs it, and writes what its audio interface
// plays to a WAV file.
//
//   n64run [options] ROM
//   n64run [--seconds S] [--wav FILE] TRACK
//
// A track (a .usf or .miniusf file) runs with the legacy players' timing and the options that its _enablecompare and
// _enableFIFOfull tags turn on. The options of the boot don't apply to it.
//
//   --seconds S     run for S seconds of emulated time (default 10, at most 86400)
//   --wav FILE      write the audio to FILE
//   --legacy        use the legacy players' timing instead of the hardware's
//   --pif FILE      a dump of the PIF ROM, which games with the CIC-NUS-6105 need
//   --8mb           give the system 8 MiB of RDRAM
//   --debug-port    turn on the debug port and print what the program writes to it
//   --controller    connect a standard controller to the first port, with nothing pressed
//   --eeprom 4k|16k connect an EEPROM of 4 or 16 Kbit to the cartridge
//   --input F:B[:X:Y] from video field F on, hold the buttons B (in hex, as the controller sends them) and put the
//                   stick at X and Y. Implies --controller. Give it once for each change.
//
// The WAV file has the rate of the first buffer. It reports when the rate changes.

#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

#include "xsflib/files.h"
#include "xsflib/wav.h"

#include "n64/system.h"
#include "usf/track.h"
#include "usf/usf_file.h"

namespace
{

const char* CicName(usf::n64::Cic cic)
{
    switch (cic)
    {
    case usf::n64::Cic::k6101:
        return "6101";
    case usf::n64::Cic::k6102:
        return "6102";
    case usf::n64::Cic::k6103:
        return "6103";
    case usf::n64::Cic::k6105:
        return "6105";
    case usf::n64::Cic::k6106:
        return "6106";
    default:
        return "unknown";
    }
}

int Usage()
{
    std::fprintf(stderr,
                 "usage: n64run [--seconds S] [--wav FILE] [--legacy] [--pif FILE] [--8mb] [--debug-port]\n"
                 "              [--controller] [--eeprom 4k|16k] [--input F:B[:X:Y]]... ROM\n"
                 "       n64run [--seconds S] [--wav FILE] TRACK\n");

    return 2;
}

// Parses F:B[:X:Y] for --input.
std::optional<usf::n64::InputEvent> ParseInput(const std::string& text)
{
    unsigned long long field = 0;
    unsigned buttons = 0;
    int x = 0;
    int y = 0;
    const int fields = std::sscanf(text.c_str(), "%llu:%x:%d:%d", &field, &buttons, &x, &y);
    if ((fields != 2 && fields != 4) || buttons > 0xFFFF || x < -128 || x > 127 || y < -128 || y > 127)
    {
        return std::nullopt;
    }

    usf::n64::InputEvent event;
    event.clock = usf::n64::InputEvent::Clock::kFields;
    event.at = field;
    event.state.buttons = static_cast<uint16_t>(buttons);
    event.state.stick_x = static_cast<int8_t>(x);
    event.state.stick_y = static_cast<int8_t>(y);

    return event;
}

} // namespace

int main(int argc, char** argv)
{
    double seconds = 10;
    std::string wav_path;
    std::string pif_path;
    std::string rom_path;
    usf::n64::Options options;
    bool controller = false;
    usf::n64::EepromType eeprom = usf::n64::EepromType::kNone;
    std::vector<usf::n64::InputEvent> inputs;
    for (int i = 1; i < argc; i++)
    {
        const std::string arg = argv[i];
        if (arg == "--seconds" && i + 1 < argc)
        {
            char* end = nullptr;
            seconds = std::strtod(argv[++i], &end);
            if (*end != '\0' || !(seconds > 0.0 && seconds <= 86400.0))
            {
                return Usage();
            }
        }
        else if (arg == "--wav" && i + 1 < argc)
        {
            wav_path = argv[++i];
        }
        else if (arg == "--pif" && i + 1 < argc)
        {
            pif_path = argv[++i];
        }
        else if (arg == "--legacy")
        {
            options.timing = usf::n64::Timing::kLegacy;
        }
        else if (arg == "--8mb")
        {
            options.rdram_size = usf::n64::kRdramSize8;
        }
        else if (arg == "--debug-port")
        {
            options.is_viewer = true;
        }
        else if (arg == "--controller")
        {
            controller = true;
        }
        else if (arg == "--eeprom" && i + 1 < argc)
        {
            const std::string size = argv[++i];
            if (size != "4k" && size != "16k")
            {
                return Usage();
            }

            eeprom = size == "4k" ? usf::n64::EepromType::k4Kbit : usf::n64::EepromType::k16Kbit;
        }
        else if (arg == "--input" && i + 1 < argc)
        {
            const std::optional<usf::n64::InputEvent> event = ParseInput(argv[++i]);
            if (!event)
            {
                return Usage();
            }

            inputs.push_back(*event);
            controller = true;
        }
        else if (!arg.empty() && arg[0] != '-' && rom_path.empty())
        {
            rom_path = arg;
        }
        else
        {
            return Usage();
        }
    }

    if (rom_path.empty())
    {
        return Usage();
    }

    std::vector<uint8_t> rom;
    if (!xsflib::ReadFile(xsflib::PathFromUtf8(rom_path), rom))
    {
        std::fprintf(stderr, "error: can't read %s\n", rom_path.c_str());
        return 1;
    }

    // A PSF file is a USF track.
    const bool track = rom.size() >= 3 && rom[0] == 'P' && rom[1] == 'S' && rom[2] == 'F';
    usf::LoadedUsf loaded;
    if (track)
    {
        if (auto error = usf::LoadUsf(rom_path, xsflib::psf::ReadWholeFile, loaded))
        {
            std::fprintf(stderr, "error: %s\n", error->c_str());
            return 1;
        }

        options = usf::TrackOptions(loaded);
    }

    usf::n64::System system(options);
    system.LoadRom(std::move(rom));
    system.GetPif().ConnectController(0, controller);
    system.GetPif().SetEeprom(eeprom);
    for (const usf::n64::InputEvent& event : inputs)
    {
        system.GetPif().AddInput(event);
    }

    if (!pif_path.empty())
    {
        std::vector<uint8_t> pif;
        if (!xsflib::ReadFile(xsflib::PathFromUtf8(pif_path), pif))
        {
            std::fprintf(stderr, "error: can't read %s\n", pif_path.c_str());
            return 1;
        }

        system.SetPifRom(std::move(pif));
    }

    std::vector<int16_t> samples;
    uint32_t rate = 0;
    uint64_t buffers = 0;
    system.SetAudioCallback(
        [&](const usf::n64::AudioBuffer& buffer)
        {
            if (rate == 0)
            {
                rate = buffer.sample_rate;
            }
            else if (buffer.sample_rate != rate)
            {
                std::fprintf(stderr, "note: the sample rate changed to %u Hz after %llu buffers\n", buffer.sample_rate,
                             static_cast<unsigned long long>(buffers));
                rate = buffer.sample_rate;
            }

            samples.insert(samples.end(), buffer.samples.begin(), buffer.samples.end());
            buffers++;
        });

    system.SetDebugOutput([](std::string_view text) { std::fwrite(text.data(), 1, text.size(), stdout); });

    bool started = true;
    if (track)
    {
        usf::StartTrack(system, loaded.rom, loaded.state);
        std::printf("track: %zu files, ROM image of %zu bytes, _enablecompare %s, _enableFIFOfull %s\n",
                    loaded.sources.size(), loaded.rom.size(), loaded.enable_compare ? "on" : "off",
                    loaded.enable_fifo_full ? "on" : "off");
    }
    else
    {
        const usf::n64::BootInfo boot = system.Boot();
        std::printf("boot: %s, CIC %s, entry 0x%08llX, after %.3f s\n",
                    boot.booted ? "reached the entry point" : "failed", CicName(boot.cic),
                    static_cast<unsigned long long>(boot.entry_point & 0xFFFFFFFF),
                    static_cast<double>(system.Now()) / static_cast<double>(system.TicksPerSecond()));
        if (!boot.problem.empty())
        {
            std::printf("boot: %s\n", boot.problem.c_str());
        }

        started = boot.booted;
    }

    system.RunSeconds(seconds);
    std::printf("ran %.3f s: %llu instructions, %llu audio buffers, %zu frames at %u Hz, PC 0x%08llX\n",
                static_cast<double>(system.Now()) / static_cast<double>(system.TicksPerSecond()),
                static_cast<unsigned long long>(system.GetCpu().InstructionCount()),
                static_cast<unsigned long long>(buffers), samples.size() / 2, rate,
                static_cast<unsigned long long>(system.GetCpu().Pc() & 0xFFFFFFFF));

    if (!wav_path.empty())
    {
        const std::vector<uint8_t> wav = xsflib::WavFile(samples, 2, rate == 0 ? 44100 : rate);
        if (auto error = xsflib::WriteFile(xsflib::PathFromUtf8(wav_path), wav))
        {
            std::fprintf(stderr, "error: %s\n", error->c_str());
            return 1;
        }
    }

    return started ? 0 : 1;
}
