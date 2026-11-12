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
//   --port P        the port that the next --controller or --input names, 1 to 4 (default 1)
//   --board B       the board: console (the default), aleck64 or aleck64-e90. The Aleck64 implies --8mb.
//   --board-input F:S:Y[:R]
//                   from video field F on, the Aleck64's inputs: the words S at 0xC0800000 and Y at 0xC0800004, and
//                   the four rows R of a mahjong panel (8 hex digits, the row of select 0x01 first). Field 0 sets
//                   the inputs at reset.
//
// The WAV file has the rate of the first buffer. It reports when the rate changes.

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "xsflib/files.h"
#include "xsflib/wav.h"

#include "n64/system.h"
#include "usf/track.h"
#include "usf/usf_file.h"

namespace
{

// A WAV file's sizes are 32-bit. The RIFF size counts 36 bytes of headers besides the audio.
constexpr uint64_t kMaxWavDataBytes = 0xFFFFFFFFu - 36;

const char* CicName(usf::n64::Cic cic)
{
    switch (cic)
    {
    case usf::n64::Cic::k6102:
        return "6102";
    case usf::n64::Cic::k6103:
        return "6103";
    case usf::n64::Cic::k6105:
        return "6105";
    case usf::n64::Cic::k6106:
        return "6106";
    case usf::n64::Cic::k5101:
        return "5101";
    default:
        return "unknown";
    }
}

int Usage()
{
    std::fprintf(stderr,
                 "usage: n64run [--seconds S] [--wav FILE] [--legacy] [--pif FILE] [--8mb] [--debug-port]\n"
                 "              [--port P] [--controller] [--eeprom 4k|16k] [--input F:B[:X:Y]]...\n"
                 "              [--board B] [--board-input F:S:Y[:R]]... ROM\n"
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

// Parses F:S:Y[:R] for --board-input.
std::optional<usf::n64::BoardInputEvent> ParseBoardInput(const std::string& text)
{
    unsigned long long field = 0;
    unsigned switches = 0;
    unsigned system = 0;
    unsigned rows = 0;
    const int fields = std::sscanf(text.c_str(), "%llu:%x:%x:%x", &field, &switches, &system, &rows);
    if (fields != 3 && fields != 4)
    {
        return std::nullopt;
    }

    usf::n64::BoardInputEvent event;
    event.field = field;
    event.inputs.switches = switches;
    event.inputs.system = system;
    event.inputs.panel = fields == 4;
    for (size_t row = 0; row < event.inputs.rows.size(); row++)
    {
        event.inputs.rows[row] = static_cast<uint8_t>(rows >> (24 - 8 * row));
    }

    return event;
}

// The command line.
struct Arguments
{
    double seconds = 10;
    std::string wav_path;
    std::string pif_path;
    std::string rom_path;
    usf::n64::Options options;
    std::array<bool, usf::n64::Pif::kPorts> controllers{};
    std::vector<usf::n64::BoardInputEvent> board_inputs;
    usf::n64::EepromType eeprom = usf::n64::EepromType::kNone;
    std::vector<usf::n64::InputEvent> inputs;

    // The options of the boot that the command line gives. A track takes none of them.
    std::vector<std::string> boot_options;
};

// Parses the name of a board for --board.
std::optional<usf::n64::Board> ParseBoard(const std::string& text)
{
    if (text == "console")
    {
        return usf::n64::Board::kConsole;
    }
    if (text == "aleck64")
    {
        return usf::n64::Board::kAleck64;
    }
    if (text == "aleck64-e90")
    {
        return usf::n64::Board::kAleck64E90;
    }

    return std::nullopt;
}

// Parses the size of an EEPROM for --eeprom.
std::optional<usf::n64::EepromType> ParseEeprom(const std::string& text)
{
    if (text == "4k")
    {
        return usf::n64::EepromType::k4Kbit;
    }
    if (text == "16k")
    {
        return usf::n64::EepromType::k16Kbit;
    }

    return std::nullopt;
}

// Parses argv[i] as an option of the boot into `args`, and moves `i` past the option's value. `port` is the port that
// --controller and --input name, and `rdram_8mb` records --8mb. Returns false for an argument that isn't an option of
// the boot, an option with no value after it, or a value that isn't valid.
bool ParseBootOption(int argc, char** argv, int& i, Arguments& args, int& port, bool& rdram_8mb)
{
    const std::string arg = argv[i];
    const bool has_value = i + 1 < argc;
    if (arg == "--pif" && has_value)
    {
        args.pif_path = argv[++i];
    }
    else if (arg == "--legacy")
    {
        args.options.timing = usf::n64::Timing::kLegacy;
    }
    else if (arg == "--8mb")
    {
        rdram_8mb = true;
    }
    else if (arg == "--debug-port")
    {
        args.options.is_viewer = true;
    }
    else if (arg == "--controller")
    {
        args.controllers[port] = true;
    }
    else if (arg == "--port" && has_value)
    {
        port = std::atoi(argv[++i]) - 1;
        if (port < 0 || port >= usf::n64::Pif::kPorts)
        {
            return false;
        }
    }
    else if (arg == "--board" && has_value)
    {
        const std::optional<usf::n64::Board> board = ParseBoard(argv[++i]);
        if (!board)
        {
            return false;
        }

        args.options.board = *board;
    }
    else if (arg == "--board-input" && has_value)
    {
        const std::optional<usf::n64::BoardInputEvent> event = ParseBoardInput(argv[++i]);
        if (!event)
        {
            return false;
        }

        args.board_inputs.push_back(*event);
    }
    else if (arg == "--eeprom" && has_value)
    {
        const std::optional<usf::n64::EepromType> eeprom = ParseEeprom(argv[++i]);
        if (!eeprom)
        {
            return false;
        }

        args.eeprom = *eeprom;
    }
    else if (arg == "--input" && has_value)
    {
        const std::optional<usf::n64::InputEvent> event = ParseInput(argv[++i]);
        if (!event)
        {
            return false;
        }

        args.inputs.push_back(*event);
        args.inputs.back().port = port;
        args.controllers[port] = true;
    }
    else
    {
        return false;
    }

    return true;
}

// Parses the command line into `args`. Returns false for an option or a value that isn't valid, or without a ROM.
bool ParseArguments(int argc, char** argv, Arguments& args)
{
    bool rdram_8mb = false;
    int port = 0;
    for (int i = 1; i < argc; i++)
    {
        const std::string arg = argv[i];
        if (arg == "--seconds" && i + 1 < argc)
        {
            char* end = nullptr;
            args.seconds = std::strtod(argv[++i], &end);
            if (*end != '\0' || !(args.seconds > 0.0 && args.seconds <= 86400.0))
            {
                return false;
            }
        }
        else if (arg == "--wav" && i + 1 < argc)
        {
            args.wav_path = argv[++i];
        }
        else if (!arg.empty() && arg[0] != '-' && args.rom_path.empty())
        {
            args.rom_path = arg;
        }
        else
        {
            if (!ParseBootOption(argc, argv, i, args, port, rdram_8mb))
            {
                return false;
            }

            args.boot_options.push_back(arg);
        }
    }

    // The RDRAM size follows the last --board. The Aleck64 has 8 MiB.
    if (rdram_8mb || args.options.board != usf::n64::Board::kConsole)
    {
        args.options.rdram_size = usf::n64::kRdramSize8;
    }

    return !args.rom_path.empty();
}

// Loads the ROM into `system` and connects the controllers, the board's inputs, the EEPROM, the input script and the
// PIF ROM that `args` gives. Returns false after printing why it can't.
bool SetUpSystem(usf::n64::System& system, std::vector<uint8_t> rom, const Arguments& args)
{
    system.LoadRom(std::move(rom));

    for (int p = 0; p < usf::n64::Pif::kPorts; p++)
    {
        system.GetPif().ConnectController(p, args.controllers[static_cast<size_t>(p)]);
    }

    for (const usf::n64::BoardInputEvent& event : args.board_inputs)
    {
        if (event.field == 0)
        {
            system.SetBoardInputs(event.inputs);
        }
        else
        {
            system.AddBoardInput(event);
        }
    }

    system.GetPif().SetEeprom(args.eeprom);

    for (const usf::n64::InputEvent& event : args.inputs)
    {
        system.GetPif().AddInput(event);
    }

    if (!args.pif_path.empty())
    {
        std::vector<uint8_t> pif;
        if (!xsflib::ReadFile(xsflib::PathFromUtf8(args.pif_path), pif))
        {
            std::fprintf(stderr, "error: can't read %s\n", args.pif_path.c_str());
            return false;
        }

        system.SetPifRom(std::move(pif));
    }

    return true;
}

// What the audio interface played. The samples are kept only for a WAV file, up to the most that one can hold.
struct Audio
{
    bool keep = false;
    bool too_long = false;
    std::vector<int16_t> samples;
    uint64_t frames = 0;
    uint32_t rate = 0;
    uint64_t buffers = 0;
};

// Records the buffers that `system` plays in `audio`. The run stops where its samples would pass what a WAV file can
// hold.
void RecordAudio(usf::n64::System& system, Audio& audio)
{
    const auto on_buffer = [&system, &audio](const usf::n64::AudioBuffer& buffer)
    {
        if (audio.rate == 0)
        {
            audio.rate = buffer.sample_rate;
        }
        else if (buffer.sample_rate != audio.rate)
        {
            std::fprintf(stderr, "note: the sample rate changed to %u Hz after %llu buffers\n", buffer.sample_rate,
                         static_cast<unsigned long long>(audio.buffers));
            audio.rate = buffer.sample_rate;
        }

        if (audio.keep && 2 * (uint64_t{audio.samples.size()} + buffer.samples.size()) > kMaxWavDataBytes)
        {
            audio.too_long = true;
            system.Stop();
            return;
        }
        if (audio.keep)
        {
            audio.samples.insert(audio.samples.end(), buffer.samples.begin(), buffer.samples.end());
        }

        audio.frames += buffer.samples.size() / 2;
        audio.buffers++;
    };
    system.SetAudioCallback(on_buffer);
}

// Writes the audio to the WAV file at `path`. Returns false after printing why it can't.
bool WriteWav(const std::string& path, const Audio& audio)
{
    if (audio.too_long)
    {
        std::fprintf(stderr, "error: a WAV file can't hold 4 GiB of audio. Run for fewer seconds.\n");
        return false;
    }

    const std::vector<uint8_t> wav = xsflib::WavFile(audio.samples, 2, audio.rate == 0 ? 44100 : audio.rate);
    if (auto error = xsflib::WriteFile(xsflib::PathFromUtf8(path), wav))
    {
        std::fprintf(stderr, "error: %s\n", error->c_str());
        return false;
    }

    return true;
}

// Loads the USF track at `args.rom_path` into `loaded`, and sets `args.options` from its tags. Returns an exit code
// after printing why it can't.
std::optional<int> LoadTrack(Arguments& args, usf::LoadedUsf& loaded)
{
    if (!args.boot_options.empty())
    {
        std::fprintf(stderr, "error: %s applies only to a ROM image\n", args.boot_options.front().c_str());
        return 2;
    }
    if (auto error = usf::LoadUsf(args.rom_path, xsflib::psf::ReadWholeFile, loaded))
    {
        std::fprintf(stderr, "error: %s\n", error->c_str());
        return 1;
    }

    args.options = usf::TrackOptions(loaded);

    return std::nullopt;
}

// Starts the track in `loaded`, or boots the ROM when `track` is false, and prints a line about it. Returns false for a
// boot that failed.
bool Start(usf::n64::System& system, bool track, const usf::LoadedUsf& loaded)
{
    if (track)
    {
        usf::StartTrack(system, loaded.rom, loaded.state);
        std::printf("track: %zu files, ROM image of %zu bytes, _enablecompare %s, _enableFIFOfull %s\n",
                    loaded.sources.size(), loaded.rom.size(), loaded.enable_compare ? "on" : "off",
                    loaded.enable_fifo_full ? "on" : "off");

        return true;
    }

    const usf::n64::BootInfo boot = system.Boot();
    std::printf("boot: %s, CIC %s, entry 0x%08llX, after %.3f s\n", boot.booted ? "reached the entry point" : "failed",
                CicName(boot.cic), static_cast<unsigned long long>(boot.entry_point & 0xFFFFFFFF),
                static_cast<double>(system.Now()) / static_cast<double>(system.TicksPerSecond()));
    if (!boot.problem.empty())
    {
        std::printf("boot: %s\n", boot.problem.c_str());
    }

    return boot.booted;
}

} // namespace

int main(int argc, char** argv)
{
    Arguments args;
    if (!ParseArguments(argc, argv, args))
    {
        return Usage();
    }

    std::vector<uint8_t> rom;
    if (!xsflib::ReadFile(xsflib::PathFromUtf8(args.rom_path), rom))
    {
        std::fprintf(stderr, "error: can't read %s\n", args.rom_path.c_str());
        return 1;
    }

    // A PSF file is a USF track.
    const bool track = rom.size() >= 3 && rom[0] == 'P' && rom[1] == 'S' && rom[2] == 'F';
    usf::LoadedUsf loaded;
    if (track)
    {
        if (const std::optional<int> exit_code = LoadTrack(args, loaded))
        {
            return *exit_code;
        }
    }

    usf::n64::System system(args.options);
    if (!SetUpSystem(system, std::move(rom), args))
    {
        return 1;
    }

    Audio audio;
    audio.keep = !args.wav_path.empty();
    RecordAudio(system, audio);
    system.SetDebugOutput([](std::string_view text) { std::fwrite(text.data(), 1, text.size(), stdout); });
    const bool started = Start(system, track, loaded);

    system.RunSeconds(args.seconds);
    std::printf("ran %.3f s: %llu instructions, %llu audio buffers, %llu frames at %u Hz, PC 0x%08llX\n",
                static_cast<double>(system.Now()) / static_cast<double>(system.TicksPerSecond()),
                static_cast<unsigned long long>(system.GetCpu().InstructionCount()),
                static_cast<unsigned long long>(audio.buffers), static_cast<unsigned long long>(audio.frames),
                audio.rate, static_cast<unsigned long long>(system.GetCpu().Pc() & 0xFFFFFFFF));

    if (!args.wav_path.empty() && !WriteWav(args.wav_path, audio))
    {
        return 1;
    }

    return started ? 0 : 1;
}
