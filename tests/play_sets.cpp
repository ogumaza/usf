// SPDX-License-Identifier: MIT

// usfplay's output against the core's. Each folder in FOLDER that holds a file named expect.txt is a set. Each line of
// expect.txt names a track without its extension and a number of seconds, separated by a tab and the word "core":
//
//   TRACK  core  SECONDS
//
// For every line, the test runs usfplay on the track for SECONDS, with no fade, and reads its WAV file. The file must
// hold what usf::Player plays, sample for sample, at its rate: the anchor of that check is the core's playback API,
// played directly and not through the front end. Lines that start with # are comments.
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

#include "xsflib/bytes.h"
#include "xsflib/files.h"
#include "xsflib/psf/psf.h"

#include "usf/player.h"

namespace
{

namespace fs = std::filesystem;

struct Wav
{
    std::vector<int16_t> samples;
    uint32_t rate = 0;
};

bool ReadWav(const fs::path& path, Wav& wav)
{
    std::vector<uint8_t> data;
    if (!xsflib::ReadFile(path, data) || data.size() < 12)
    {
        return false;
    }

    size_t pos = 12;
    bool pcm16 = false;
    while (pos + 8 <= data.size())
    {
        const std::string id(reinterpret_cast<const char*>(data.data() + pos), 4);
        const size_t size = xsflib::LoadLe32(data.data() + pos + 4);
        const size_t body = pos + 8;
        if (body + size > data.size())
        {
            return false;
        }

        if (id == "fmt " && size >= 16)
        {
            pcm16 = xsflib::LoadLe16(data.data() + body) == 1 && xsflib::LoadLe16(data.data() + body + 14) == 16;
            wav.rate = xsflib::LoadLe32(data.data() + body + 4);
        }
        else if (id == "data" && pcm16)
        {
            wav.samples.resize(size / 2);
            for (size_t i = 0; i < wav.samples.size(); i++)
            {
                wav.samples[i] = static_cast<int16_t>(xsflib::LoadLe16(data.data() + body + 2 * i));
            }
            return true;
        }

        pos = body + size + (size & 1);
    }

    return false;
}

// What the core's playback API plays for `frames` frames, with no front end.
bool PlayOnCore(const fs::path& path, uint64_t frames, Wav& wav)
{
    usf::Player player;
    if (!player.Load(xsflib::Utf8(path), xsflib::psf::ReadWholeFile) || !player.Start())
    {
        std::printf("%s\n", player.Error().c_str());
        return false;
    }

    wav.rate = player.SampleRate();
    wav.samples.resize(2 * static_cast<size_t>(frames));
    uint64_t done = 0;
    while (done < frames)
    {
        // A render returns less than it was asked for only at a change of rate, or after an error.
        const size_t left = static_cast<size_t>(frames - done);
        const size_t count = player.Render(wav.samples.data() + 2 * static_cast<size_t>(done), left);
        if (count == 0)
        {
            break;
        }

        done += count;
    }

    wav.samples.resize(2 * static_cast<size_t>(done));

    return done == frames;
}

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

bool CheckTrack(const fs::path& set, const std::string& line, const fs::path& usfplay, const fs::path& work)
{
    std::vector<std::string> fields;
    size_t start = 0;
    while (start <= line.size())
    {
        const size_t end = std::min(line.find('\t', start), line.size());
        fields.push_back(line.substr(start, end - start));
        start = end + 1;
    }

    if (fields.size() != 3 || fields[1] != "core")
    {
        std::printf("FAIL %s: can't read the line\n", line.c_str());
        return false;
    }

    // The renderer, for the line's seconds.
    const std::string& name = fields[0];
    const double seconds = std::atof(fields[2].c_str());
    const fs::path track = set / xsflib::PathFromUtf8(name + ".miniusf");
    const fs::path out = work / "render.wav";
    Wav rendered;
    if (!RunUsfplay(usfplay, track, out, fields[2]) || !ReadWav(out, rendered))
    {
        std::printf("FAIL %s: usfplay gave no WAV file\n", name.c_str());
        return false;
    }

    // Against the core.
    const uint64_t frames = rendered.samples.size() / 2;
    Wav core;
    if (!PlayOnCore(track, frames, core) || rendered.rate != core.rate)
    {
        std::printf("FAIL %s: the core played %.3f s at %u Hz, usfplay wrote %.3f s at %u Hz\n", name.c_str(),
                    static_cast<double>(core.samples.size() / 2) / std::max<uint32_t>(core.rate, 1), core.rate,
                    static_cast<double>(frames) / std::max<uint32_t>(rendered.rate, 1), rendered.rate);
        return false;
    }

    if (!std::equal(rendered.samples.begin(), rendered.samples.end(), core.samples.begin()))
    {
        const auto differ = std::mismatch(rendered.samples.begin(), rendered.samples.end(), core.samples.begin());
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

    std::vector<fs::path> sets;
    for (const auto& entry : fs::recursive_directory_iterator(folder, error))
    {
        if (entry.path().filename() == "expect.txt")
        {
            sets.push_back(entry.path().parent_path());
        }
    }
    std::sort(sets.begin(), sets.end());

    int checked = 0;
    int failed = 0;
    for (const fs::path& set : sets)
    {
        std::vector<uint8_t> text;
        xsflib::ReadFile(set / "expect.txt", text);
        std::string line;
        for (size_t i = 0; i <= text.size(); i++)
        {
            if (i < text.size() && text[i] != '\n')
            {
                line += static_cast<char>(text[i]);
                continue;
            }

            if (!line.empty() && line.back() == '\r')
            {
                line.pop_back();
            }

            if (!line.empty() && line[0] != '#')
            {
                checked++;
                failed += CheckTrack(set, line, usfplay, work) ? 0 : 1;
            }

            line.clear();
        }
    }

    fs::remove_all(work, error);
    std::printf("\n%d of %d tracks failed\n", failed, checked);

    return checked > 0 && failed == 0 ? 0 : 1;
}
