// SPDX-License-Identifier: MIT

// usfrip: rips a Nintendo 64 game's music into a USF set: one .usflib and one .miniusf for each track. It runs the
// game's code on the emulator core, with the game's profile (src/rip/profile.h) saying where to stop and how each track
// is chosen.
//
// usage: usfrip [options] <image> [<image> ...]

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <functional>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "xsflib/cli/parallel.h"
#include "xsflib/cli/rip.h"
#include "xsflib/files.h"
#include "xsflib/rip/file_names.h"
#include "xsflib/rip/rom_set.h"
#include "xsflib/rip/tool.h"
#include "xsflib/sha1.h"

#include "n64/constants.h"
#include "n64/system.h"
#include "rip/profiles.h"
#include "rip/rip.h"
#include "rip/rom_set.h"
#include "rip/snapshot.h"
#include "rip/trim.h"

namespace
{

namespace fs = std::filesystem;

using usf::rip::GameProfile;
using usf::rip::Track;
using usf::rip::TrackRun;

// The extensions of a cartridge image's file, one for each of its three byte orders.
constexpr std::string_view kImageExtensions[] = {".z64", ".v64", ".n64"};

constexpr char kUsage[] =
    "usage: usfrip [options] <image> [<image> ...]\n"
    "\n"
    "Rips each Nintendo 64 image into a .usflib and a .miniusf for each track, in a folder next to it\n"
    "that's named after it with \"_usf\" added. The image may be in any byte order (.z64, .v64 or .n64),\n"
    "and it may be in a ZIP archive. A game of the Aleck64 arcade board comes as its ROM set instead: a\n"
    "ZIP archive or a folder of its chips. usfrip rips only games that it has a profile for, found by\n"
    "the image's SHA-1. It boots the game, runs it to the profile's snapshot point and saves the state\n"
    "there. Each track then runs from that state for its length analysis and for the coverage that trims\n"
    "the set. Each mini is played back at the end and compared with the untrimmed state's output. An\n"
    "image with more than one set, such as a game's music and its sound effects, gets a folder for each\n"
    "set inside that folder.\n"
    "\n"
    "options:\n"
    "  -o, --output DIR     put the rips in DIR\n"
    "  --list               list the tracks of each set, then stop\n"
    "  --only REGEX         rip only the tracks whose name matches\n"
    "  --bgm                rip only the music, without the sets of sound effects\n"
    "  --no-length          skip the length analysis and omit length and fade tags, except where\n"
    "                       the profile gives a track's length\n"
    "  --no-trim            keep the whole ROM and the whole state in the library\n"
    "  --no-check           don't play the minis back to compare them with the untrimmed state\n"
    "  --margin S           seconds of play after a track's fade that the coverage still covers\n"
    "                       (default 10)\n"
    "  --pif FILE           a dump of the console's PIF ROM, which games with the CIC-NUS-6105 need\n"
    "  --jobs N             the number of tracks that run at once (default: the number of CPU\n"
    "                       threads)\n"
    "  --game NAME, --artist NAME, --year YEAR, --copyright TEXT, --by NAME\n"
    "                       tags (--by defaults to usfrip, the others to the profile's)\n"
    "  --version            show the version\n"
    "  -h, --help           show this help\n";

struct Options : xsflib::cli::RipOptions
{
    bool trim = true;
    bool check = true;
    double margin = 10.0;
    std::string pif_path;
    std::vector<uint8_t> pif_rom;
};

int ParseOption(const std::string& option, const std::function<std::string()>& next, Options& o)
{
    if (option == "--no-trim")
    {
        o.trim = false;
    }
    else if (option == "--no-check")
    {
        o.check = false;
    }
    else if (option == "--pif")
    {
        o.pif_path = next();
    }
    else if (option == "--margin")
    {
        const std::string value = next();
        char* end = nullptr;
        const double margin = std::strtod(value.c_str(), &end);
        if (value.empty() || *end != '\0' || !(margin >= 0.0 && margin <= 3600.0))
        {
            std::fprintf(stderr, "error: --margin takes seconds from 0 to 3600, not '%s'\n", value.c_str());
            return 2;
        }

        o.margin = margin;
    }
    else
    {
        return -1;
    }

    return 0;
}

int BeforeInputs(std::vector<std::string>&, Options& o)
{
    if (!o.pif_path.empty() && !xsflib::ReadFile(xsflib::PathFromUtf8(o.pif_path), o.pif_rom))
    {
        std::fprintf(stderr, "error: can't read %s\n", o.pif_path.c_str());
        return 1;
    }

    return 0;
}

// The image's SHA-1 in big-endian byte order, which the profiles name.
std::string ImageSha1(std::vector<uint8_t> rom)
{
    usf::n64::ToConsoleOrder(rom);
    return xsflib::Sha1::Hex(xsflib::Sha1::Of(rom));
}

// A track to rip and what its run found.
struct Job
{
    size_t track = 0;    // In the profile's tracks.
    size_t position = 0; // In the set's jobs. The set's minis are in the same order.
    uint32_t index = 0;  // The number of the track's mini: its place in the profile's tracks, from 1.
    std::string label;
    std::optional<xsflib::rip::Timing> timing;
    TrackRun run;
};

// The jobs of a set: its tracks that --only picks.
std::vector<Job> SetJobs(const GameProfile& profile, const Options& o)
{
    std::vector<Job> jobs;
    for (size_t i = 0; i < profile.tracks.size(); i++)
    {
        if (std::regex_search(profile.tracks[i].name, o.only))
        {
            Job job;
            job.track = i;
            job.position = jobs.size();
            job.index = static_cast<uint32_t>(i + 1);
            job.label = profile.tracks[i].name;
            jobs.push_back(std::move(job));
        }
    }

    return jobs;
}

// Prints a warning for each track whose run found a problem: no sound, a read of osTvType, or an access to the board.
void WarnAboutRuns(const std::vector<Job>& jobs, const Options& o)
{
    for (const Job& job : jobs)
    {
        if (job.timing && job.timing->silent)
        {
            std::fprintf(stderr, "warning: %s makes no sound\n", job.label.c_str());
        }

        // libultra keeps osTvType at 0x300. One legacy player writes 0 there at the start of a track. libultra reads 0
        // as PAL.
        if (o.trim && job.run.words.rdram.Has(usf::n64::kOsTvTypeAddress / 4))
        {
            std::fprintf(stderr, "warning: %s reads osTvType (RDRAM 0x%X). A legacy player may play it as PAL.\n",
                         job.label.c_str(), usf::n64::kOsTvTypeAddress);
        }

        // A board's game may still use the board after the snapshot. The players have nothing there, so the track can
        // sound different from the game.
        if (job.run.board_accesses > 0)
        {
            std::fprintf(stderr,
                         "warning: %s reads or writes the board's addresses after the snapshot (%llu times). Players "
                         "have nothing there.\n",
                         job.label.c_str(), static_cast<unsigned long long>(job.run.board_accesses));
        }
    }
}

// Writes the library and the minis of `set` into `out_dir`, and adds the path of each mini to `mini_paths`. Returns 1
// if a file can't be written, or 0.
int WriteSet(const GameProfile& profile, const usf::rip::SetSections& set, const std::vector<Job>& jobs,
             const fs::path& out_dir, const Options& o, xsflib::cli::FileNameRegistries& names,
             std::vector<fs::path>& mini_paths)
{
    xsflib::rip::FileNames& out_names = xsflib::cli::NamesIn(names, out_dir, ".usflib", ".miniusf");

    xsflib::cli::RipOptions tag_options = o;
    tag_options.artist = o.artist.empty() ? profile.artist : o.artist;
    tag_options.year = o.year.empty() ? profile.year : o.year;
    tag_options.copyright = o.copyright.empty() ? profile.copyright : o.copyright;
    const std::string game = o.game_name.empty() ? profile.game : o.game_name;
    usf::rip::SetTags tags;
    tags.common = xsflib::cli::RipTags(tag_options, game, "usf", nullptr);
    tags.ripper = std::string("usfrip ") + USF_VERSION;

    const std::vector<uint8_t> library = usf::rip::LibraryFile(profile, set.library, tags);
    const std::string library_name = out_names.LibraryIn(out_dir, game, library);
    if (!xsflib::rip::WriteFile(out_dir / xsflib::PathFromUtf8(library_name), library))
    {
        std::fprintf(stderr, "error: can't write %s\n", library_name.c_str());
        return 1;
    }

    std::fprintf(stderr, "%s  %zu bytes\n", library_name.c_str(), library.size());

    // The minis, each with the sections at its job's position.
    const auto build = [&](const Job& job, const xsflib::psf::Tags& mini_tags)
    {
        const usf::Sections& sections = set.minis[job.position];
        return usf::rip::MiniFile(profile, sections, job.index, library_name, tags.ripper, mini_tags);
    };
    xsflib::cli::MiniSettings settings;
    settings.sizes = true;
    settings.paths = &mini_paths;

    return xsflib::cli::WriteMinis(out_dir, out_names, jobs, tags.common, settings, build) < 0 ? 1 : 0;
}

// Each mini, played back with the library, must give what its untrimmed state gave. Returns 1 if a mini differs, or 0.
int CheckMinis(const std::vector<Job>& jobs, const std::vector<fs::path>& mini_paths, const Options& o)
{
    // An exception from a check, such as running out of memory, becomes the error of that mini.
    std::vector<std::optional<std::string>> problems(jobs.size());
    const auto check = [&](size_t i)
    {
        try
        {
            problems[i] = usf::rip::CheckTrack(xsflib::Utf8(mini_paths[i]), jobs[i].run);
        }
        catch (const std::exception& e)
        {
            problems[i] = e.what();
        }
    };
    xsflib::cli::RunParallel(jobs.size(), o.jobs, check);

    int result = 0;
    size_t same = 0;
    for (size_t i = 0; i < jobs.size(); i++)
    {
        if (problems[i])
        {
            std::fprintf(stderr, "error: %s: %s\n", xsflib::Utf8(mini_paths[i].filename()).c_str(),
                         problems[i]->c_str());
            result = 1;
        }
        else
        {
            same++;
        }
    }
    std::fprintf(stderr, "%zu of %zu minis play the same as the untrimmed state\n", same, jobs.size());

    return result;
}

// Rips one set of the image into `out_dir`.
int RipSet(std::vector<uint8_t> rom, const GameProfile& profile, std::vector<Job> jobs, const fs::path& out_dir,
           const Options& o, xsflib::cli::FileNameRegistries& names)
{
    const usf::rip::Snapshot snapshot = usf::rip::TakeSnapshot(profile, std::move(rom), o.pif_rom);
    std::fprintf(stderr, "snapshot at %.3f s, arrival %d\n", snapshot.seconds, snapshot.arrival);

    // Each track runs from the snapshot.
    usf::rip::RunSettings settings;
    settings.lengths = o.lengths;
    settings.coverage = o.trim;
    settings.reference = o.check;
    settings.margin_seconds = o.margin;
    const auto analyze = [&](Job& job)
    {
        job.run = usf::rip::RunTrack(profile, snapshot, profile.tracks[job.track], settings);
        job.timing = job.run.timing;

        return o.lengths || profile.tracks[job.track].length_seconds.has_value();
    };
    xsflib::cli::AnalysisSettings analysis;
    analysis.progress = "ran";
    xsflib::cli::AnalyzeLengths(jobs, o.jobs, analyze, analysis);
    WarnAboutRuns(jobs, o);

    // The set.
    std::vector<usf::rip::Track> tracks;
    std::vector<usf::rip::TrackWords> words;
    for (const Job& job : jobs)
    {
        tracks.push_back(profile.tracks[job.track]);
        words.push_back(job.run.words);
    }
    const usf::rip::SetSections set = usf::rip::BuildSet(snapshot, tracks, o.trim ? &words : nullptr);

    std::error_code error;
    fs::create_directories(out_dir, error);
    if (error)
    {
        std::fprintf(stderr, "error: can't create %s: %s\n", xsflib::Utf8(out_dir).c_str(), error.message().c_str());
        return 1;
    }

    std::vector<fs::path> mini_paths;
    if (WriteSet(profile, set, jobs, out_dir, o, names, mini_paths) != 0)
    {
        return 1;
    }

    const int result = o.check ? CheckMinis(jobs, mini_paths, o) : 0;
    xsflib::rip::ReportOutput(out_dir);

    return result;
}

// Puts together the image of the first game whose chips are all in the ROM set at `path`. A ZIP archive may contain one
// cartridge image instead. Returns false after printing why there's no image.
bool ReadSetImage(const std::string& input, const fs::path& path, std::vector<uint8_t>& rom)
{
    std::vector<usf::rip::SetFile> files;
    if (auto problem = usf::rip::ReadRomSet(path, usf::rip::Profiles(), files))
    {
        std::fprintf(stderr, "error: %s\n", problem->c_str());
        return false;
    }

    // A ZIP archive may hold a cartridge image instead, as images are often given. A set with some of a game's chips
    // keeps its message about the missing ones, unless it holds an image too.
    usf::rip::RomSetImage image = usf::rip::BuildRomSetImage(usf::rip::Profiles(), files);
    std::error_code error;
    usf::rip::SetFile* cartridge = nullptr;
    if (image.profile == nullptr && !fs::is_directory(path, error))
    {
        cartridge = xsflib::rip::OnlyFileWithExtension(files, kImageExtensions);
    }

    if (image.profile != nullptr)
    {
        rom = std::move(image.rom);

        if (!image.others.empty())
        {
            std::string other_games;
            for (const GameProfile* other : image.others)
            {
                other_games += (other_games.empty() ? "" : ", ") + other->game;
            }

            std::fprintf(stderr,
                         "warning: %s contains the chips of more than one game. Only %s is ripped. Not ripped: %s. "
                         "Put each game in a folder or an archive of its own.\n",
                         input.c_str(), image.profile->game.c_str(), other_games.c_str());
        }
    }
    else if (cartridge != nullptr)
    {
        rom = std::move(cartridge->data);
    }
    else
    {
        std::fprintf(stderr, "error: %s: %s\n", input.c_str(), image.problem.c_str());
        return false;
    }

    return true;
}

// Reads the image at `input` into `rom`: a cartridge image, or an arcade board's ROM set put together from its chips.
// Returns false after printing why it can't.
bool ReadImage(const std::string& input, const fs::path& path, std::vector<uint8_t>& rom)
{
    if (usf::rip::IsRomSet(path))
    {
        return ReadSetImage(input, path, rom);
    }
    if (!xsflib::ReadFile(path, rom))
    {
        std::fprintf(stderr, "error: can't read %s\n", input.c_str());
        return false;
    }

    return true;
}

// Prints the tracks of a set for --list.
void ListTracks(const GameProfile& profile)
{
    for (size_t i = 0; i < profile.tracks.size(); i++)
    {
        const Track& track = profile.tracks[i];
        std::printf("%4zu  0x%X  %s\n", i + 1, track.number, track.name.c_str());
    }
    std::fflush(stdout);
}

int RipInput(const std::string& input, const Options& o, xsflib::cli::FileNameRegistries& names)
{
    std::vector<uint8_t> rom;
    const fs::path path = xsflib::PathFromUtf8(input);
    if (!ReadImage(input, path, rom))
    {
        return 1;
    }

    const std::string sha1 = ImageSha1(rom);
    const std::vector<const GameProfile*> profiles = usf::rip::FindProfiles(sha1);
    if (profiles.empty())
    {
        std::fprintf(stderr, "error: %s: no profile for this image (SHA-1 %s)\n", input.c_str(), sha1.c_str());
        return 1;
    }

    // An image with more than one set puts each set in a folder named after the set.
    const bool several = profiles.size() > 1;
    const fs::path base_dir = o.out_dir.empty() ? xsflib::rip::DefaultOutputDir(input, "_usf") : fs::path(o.out_dir);
    int result = 0;
    bool any_track = false;
    bool any_set = false;
    for (const GameProfile* profile : profiles)
    {
        if (o.bgm && profile->sound_effects)
        {
            continue;
        }

        any_set = true;
        if (several)
        {
            std::fprintf(stderr, "%s: %s (%s), %s\n", input.c_str(), profile->game.c_str(), profile->serial.c_str(),
                         profile->set.c_str());
        }
        else
        {
            std::fprintf(stderr, "%s: %s (%s)\n", input.c_str(), profile->game.c_str(), profile->serial.c_str());
        }

        if (o.list)
        {
            ListTracks(*profile);
            continue;
        }

        std::vector<Job> jobs = SetJobs(*profile, o);
        if (jobs.empty())
        {
            continue;
        }

        any_track = true;
        const fs::path out_dir = several ? base_dir / xsflib::PathFromUtf8(profile->set) : base_dir;
        result = std::max(result, RipSet(rom, *profile, std::move(jobs), out_dir, o, names));
    }

    if (!any_set)
    {
        std::fprintf(stderr, "error: %s: the image has no set of music\n", input.c_str());
        return 1;
    }
    if (!o.list && !any_track)
    {
        std::fprintf(stderr, "error: no track matches '%s'\n", o.only_text.c_str());
        return 1;
    }

    return result;
}

} // namespace

int main(int argc, char** argv)
{
    xsflib::cli::RipTool<Options> tool;
    tool.name = "usfrip";
    tool.version = USF_VERSION;
    tool.usage = kUsage;
    tool.modes = false;
    tool.archives = false;
    tool.parse_option = ParseOption;
    tool.before_inputs = BeforeInputs;
    tool.rip_input = RipInput;

    return xsflib::cli::RunRipTool(tool, argc, argv);
}
