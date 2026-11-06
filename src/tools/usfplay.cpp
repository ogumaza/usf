// SPDX-License-Identifier: MIT

// usfplay: renders .miniusf and .usf files to WAV or FLAC at each track's rate.
//
// usage: usfplay [options] <file> [<file> ...]
//   -o, --output FILE  the output file, WAV or FLAC by its extension (default: the input's name with .wav, in the same
//                      folder). Only valid with a single input.
//   --length T         override the length tag (seconds, m:s or h:m:s)
//   --fade T           override the fade tag
//   --default T        the length when there's no length tag (default and fade: PlaybackOptions)
//   --info             print the tags and loaded files without rendering
//   --version          print the version
//
// The files are rendered one after another. A file that fails doesn't stop the rest. A .usflib among the inputs is
// skipped. It's a library that the other files load. Files dropped on the program become inputs.
//
// The tool runs on xsflib's play-tool frame (xsflib/cli/play.h). This file holds its names, help and --info summary.

#include <string>

#include "xsflib/cli/play.h"

#include "usf/uniform_rate_player.h"

namespace
{

using Playback = xsflib::player::Playback<usf::UniformRatePlayer>;

// The help's description of what the tool does.
constexpr char kDescription[] =
    "Renders each .miniusf or .usf file to a WAV file next to it at the rate that the game's code\n"
    "gives its audio interface. The length and fade come from the file's tags. Libraries (.usflib)\n"
    "are skipped because the .miniusf files load them.\n"
    "You can also drop files on the program.\n";

// What --info prints about a set: its files, its ROM image, its flags and its rate. The frame doesn't let this start
// the playback's player, so a second player runs the set up to its first frames.
std::string DescribeSet(Playback& playback)
{
    const usf::LoadedUsf& set = playback.GetPlayer().GetLoadedSet();
    std::string text = std::to_string(set.sources.size()) + " files, a ROM image of " + std::to_string(set.rom.size()) +
                       " bytes, _enablecompare " + (set.enable_compare ? "on" : "off") + ", _enableFIFOfull " +
                       (set.enable_fifo_full ? "on" : "off");

    usf::UniformRatePlayer probe;
    if (probe.Load(set) && probe.Start())
    {
        text += ", " + std::to_string(probe.SampleRate()) + " Hz";
    }

    return text;
}

} // namespace

int main(int argc, char** argv)
{
    xsflib::cli::PlayTool<usf::UniformRatePlayer> tool;
    tool.name = "usfplay";
    tool.version = USF_VERSION;
    tool.description = kDescription;
    tool.output_option = "write FILE instead. Its extension selects WAV or FLAC (one input only)";
    tool.info_option = "print the tags and the files that each input loads, without rendering";
    tool.library_extension = ".usflib";
    tool.mini_extension = ".miniusf";
    tool.a_file = "a USF file";
    tool.set_up = "start-up";
    tool.describe_set = DescribeSet;

    return xsflib::cli::RunPlayTool(tool, argc, argv);
}
