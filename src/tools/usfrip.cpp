// SPDX-License-Identifier: MIT

// usfrip: rips a Nintendo 64 game's music into a USF set: one .usflib and one .miniusf for each track. It runs the
// game's code on the emulator core, with the game's profile (src/rip/profile.h) saying where to stop and how each
// track is chosen.
//
// usage: usfrip [options] <image> [<image> ...]

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "xsflib/cli/rip.h"
#include "xsflib/files.h"
#include "xsflib/rip/file_names.h"
#include "xsflib/rip/tool.h"
#include "xsflib/sha1.h"

#include "rip/profiles.h"
#include "rip/rip.h"
#include "rip/snapshot.h"
#include "rip/trim.h"

namespace
{

namespace fs = std::filesystem;

constexpr uint32_t kOsTvType = 0x300;

using usf::rip::GameProfile;
using usf::rip::Track;
using usf::rip::TrackRun;

constexpr char kUsage[] =
    "usage: usfrip [options] <image> [<image> ...]\n"
    "\n"
    "Rips each Nintendo 64 image into a .usflib and a .miniusf for each track, in a folder next to it\n"
    "that's named after it with \"_usf\" added. The image may be in any byte order (.z64, .v64 or\n"
    ".n64). usfrip rips only games that it has a profile for, found by the image's SHA-1. It boots\n"
    "the game, runs it to the profile's snapshot point and saves the state there. Each track then\n"
    "runs from that state for its length analysis and for the coverage that trims the set. Each\n"
    "mini is played back at the end and compared with the untrimmed state's output.\n"
    "\n"
    "options:\n"
    "  -o, --output DIR     put the rips in DIR\n"
    "  --list               list the profile's tracks, then stop\n"
    "  --only REGEX         rip only the tracks whose name matches\n"
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
    rom.resize((rom.size() + 3) & ~size_t{3}, 0);
    if (rom.size() >= 4 && rom[0] == 0x37 && rom[1] == 0x80)
    {
        for (size_t i = 0; i + 1 < rom.size(); i += 2)
        {
            std::swap(rom[i], rom[i + 1]);
        }
    }
    else if (rom.size() >= 4 && rom[0] == 0x40 && rom[1] == 0x12)
    {
        for (size_t i = 0; i + 3 < rom.size(); i += 4)
        {
            std::swap(rom[i], rom[i + 3]);
            std::swap(rom[i + 1], rom[i + 2]);
        }
    }

    return xsflib::Sha1::Hex(xsflib::Sha1::Of(rom));
}

// A track to rip and what its run found.
struct Job
{
    size_t index = 0; // In the profile's tracks.
    std::string label;
    std::optional<xsflib::rip::Timing> timing;
    TrackRun run;
};

// Runs `work(i)` for each i below `count`, on up to `threads` threads.
template <typename Work>
void RunParallel(size_t count, unsigned threads, const Work& work)
{
    std::atomic<size_t> next{0};
    const auto worker = [&]
    {
        for (size_t i; (i = next++) < count;)
        {
            work(i);
        }
    };

    std::vector<std::thread> pool;
    for (unsigned t = 1; t < threads && t < count; t++)
    {
        pool.emplace_back(worker);
    }

    worker();
    for (std::thread& thread : pool)
    {
        thread.join();
    }
}

int RipInput(const std::string& input, const Options& o, xsflib::cli::FileNameRegistries& names)
{
    std::vector<uint8_t> rom;
    if (!xsflib::ReadFile(xsflib::PathFromUtf8(input), rom))
    {
        std::fprintf(stderr, "error: can't read %s\n", input.c_str());
        return 1;
    }

    const std::string sha1 = ImageSha1(rom);
    const GameProfile* profile = usf::rip::FindProfile(sha1);
    if (profile == nullptr)
    {
        std::fprintf(stderr, "error: %s: no profile for this image (SHA-1 %s)\n", input.c_str(), sha1.c_str());
        return 1;
    }

    std::fprintf(stderr, "%s: %s (%s)\n", input.c_str(), profile->game.c_str(), profile->serial.c_str());

    if (o.list)
    {
        for (size_t i = 0; i < profile->tracks.size(); i++)
        {
            const Track& track = profile->tracks[i];
            std::printf("%4zu  0x%X  %s\n", i + 1, track.number, track.name.c_str());
        }
        return 0;
    }

    std::vector<Job> jobs;
    for (size_t i = 0; i < profile->tracks.size(); i++)
    {
        if (std::regex_search(profile->tracks[i].name, o.only))
        {
            Job job;
            job.index = i;
            job.label = profile->tracks[i].name;
            jobs.push_back(std::move(job));
        }
    }
    if (jobs.empty())
    {
        std::fprintf(stderr, "error: no track matches '%s'\n", o.only_text.c_str());
        return 1;
    }

    const usf::rip::Snapshot snapshot = usf::rip::TakeSnapshot(*profile, std::move(rom), o.pif_rom);
    std::fprintf(stderr, "snapshot at %.3f s, arrival %d\n", snapshot.seconds, snapshot.arrival);

    // Each track runs from the snapshot.
    usf::rip::RunSettings settings;
    settings.lengths = o.lengths;
    settings.coverage = o.trim;
    settings.reference = o.check;
    settings.margin_seconds = o.margin;
    const auto analyze = [&](Job& job)
    {
        job.run = usf::rip::RunTrack(*profile, snapshot, profile->tracks[job.index], settings);
        job.timing = job.run.timing;

        return o.lengths || profile->tracks[job.index].length_seconds.has_value();
    };
    xsflib::cli::AnalysisSettings analysis;
    analysis.progress = "ran";
    xsflib::cli::AnalyzeLengths(jobs, o.jobs, analyze, analysis);

    for (const Job& job : jobs)
    {
        if (job.timing && job.timing->silent)
        {
            std::fprintf(stderr, "warning: %s makes no sound\n", job.label.c_str());
        }

        // libultra keeps osTvType at 0x300. One legacy player writes 0 there at the start of a track. libultra reads 0
        // as PAL.
        if (o.trim && job.run.words.rdram.Has(kOsTvType / 4))
        {
            std::fprintf(stderr, "warning: %s reads osTvType (RDRAM 0x300). A legacy player may play it as PAL.\n",
                         job.label.c_str());
        }
    }

    // The set.
    std::vector<usf::rip::Track> tracks;
    std::vector<usf::rip::TrackWords> words;
    for (const Job& job : jobs)
    {
        tracks.push_back(profile->tracks[job.index]);
        words.push_back(job.run.words);
    }
    const usf::rip::SetSections set = usf::rip::BuildSet(snapshot, tracks, o.trim ? &words : nullptr);

    const fs::path out_dir = o.out_dir.empty() ? xsflib::rip::DefaultOutputDir(input, "_usf") : fs::path(o.out_dir);
    std::error_code error;
    fs::create_directories(out_dir, error);
    xsflib::rip::FileNames& out_names = xsflib::cli::NamesIn(names, out_dir, ".usflib", ".miniusf");

    xsflib::cli::RipOptions tag_options = o;
    tag_options.artist = o.artist.empty() ? profile->artist : o.artist;
    tag_options.year = o.year.empty() ? profile->year : o.year;
    tag_options.copyright = o.copyright.empty() ? profile->copyright : o.copyright;
    const std::string game = o.game_name.empty() ? profile->game : o.game_name;
    usf::rip::SetTags tags;
    tags.common = xsflib::cli::RipTags(tag_options, game, "usf", nullptr);
    tags.ripper = std::string("usfrip ") + USF_VERSION;

    const std::vector<uint8_t> library = usf::rip::LibraryFile(*profile, set.library, tags);
    const std::string library_name = out_names.LibraryIn(out_dir, game, library);
    if (!xsflib::rip::WriteFile(out_dir / xsflib::PathFromUtf8(library_name), library))
    {
        std::fprintf(stderr, "error: can't write %s\n", library_name.c_str());
        return 1;
    }

    std::fprintf(stderr, "%s  %zu bytes\n", library_name.c_str(), library.size());

    std::vector<fs::path> mini_paths;
    for (size_t i = 0; i < jobs.size(); i++)
    {
        const Track& track = profile->tracks[jobs[i].index];
        const std::vector<uint8_t> mini =
            usf::rip::MiniFile(*profile, set.minis[i], track, jobs[i].index, jobs[i].timing, library_name, tags);
        const std::string name = out_names.Mini(static_cast<uint32_t>(jobs[i].index + 1), track.name);
        mini_paths.push_back(out_dir / xsflib::PathFromUtf8(name));
        if (!xsflib::rip::WriteFile(mini_paths.back(), mini))
        {
            std::fprintf(stderr, "error: can't write %s\n", name.c_str());
            return 1;
        }

        const std::string length = jobs[i].timing ? xsflib::psf::FormatTime(jobs[i].timing->length_ms) : "";
        std::fprintf(stderr, "%s  %zu bytes%s%s\n", name.c_str(), mini.size(), length.empty() ? "" : "  length ",
                     length.c_str());
    }

    // Each mini, played back with the library, must give what its untrimmed state gave.
    int result = 0;
    if (o.check)
    {
        std::vector<std::optional<std::string>> problems(jobs.size());
        RunParallel(jobs.size(), o.jobs,
                    [&](size_t i) { problems[i] = usf::rip::CheckTrack(xsflib::Utf8(mini_paths[i]), jobs[i].run); });

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
    }

    xsflib::rip::ReportOutput(out_dir);

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
