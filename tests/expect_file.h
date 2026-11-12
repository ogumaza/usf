// SPDX-License-Identifier: MIT

// The expect.txt files of the tests that play USF sets, the WAV files that those tests compare, and the core's playback
// of a track. Each folder that holds a file named expect.txt is a set. Each line of expect.txt names a track without
// its extension, a check and a number of seconds, separated by tabs:
//
//   TRACK  identical  SECONDS
//   TRACK  similar    SECONDS  CORRELATION
//   TRACK  core       SECONDS
//
// "identical" and "similar" compare the track with a reference render beside it. "core" has no render. Lines that start
// with # are comments.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <string>
#include <system_error>
#include <vector>

#include "seqlib/read_error.h"
#include "seqlib/wav.h"
#include "xsflib/files.h"
#include "xsflib/psf/psf.h"
#include "xsflib/testing/sets.h"

#include "usf/player.h"

namespace usf::testing
{

// A line of an expect.txt.
struct ExpectLine
{
    std::filesystem::path set; // The set's folder.
    std::string text;          // The line as the file holds it.
    std::vector<std::string> fields;
};

// The fields of `line`, split at its tabs.
inline std::vector<std::string> SplitFields(const std::string& line)
{
    std::vector<std::string> fields;
    size_t start = 0;
    while (start <= line.size())
    {
        const size_t end = std::min(line.find('\t', start), line.size());
        fields.push_back(line.substr(start, end - start));
        start = end + 1;
    }

    return fields;
}

// Whether `fields` make a line of one of the three kinds.
inline bool IsExpectLine(const std::vector<std::string>& fields)
{
    return (fields.size() == 3 && (fields[1] == "identical" || fields[1] == "core")) ||
           (fields.size() == 4 && fields[1] == "similar");
}

// The lines of every expect.txt below `folder`, set by set in the order of the sets' paths. Comments and empty lines
// are left out. `unread` counts the folders and sets that can't be read, and a line for each is printed.
inline std::vector<ExpectLine> ReadExpectLines(const std::filesystem::path& folder, int& unread)
{
    std::error_code error;
    std::vector<std::filesystem::path> sets;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(folder, error))
    {
        if (entry.path().filename() == "expect.txt")
        {
            sets.push_back(entry.path().parent_path());
        }
    }
    if (error)
    {
        std::printf("FAIL %s: can't list the sets\n", xsflib::Utf8(folder).c_str());
        unread++;
    }
    std::sort(sets.begin(), sets.end());

    std::vector<ExpectLine> lines;
    for (const std::filesystem::path& set : sets)
    {
        std::vector<uint8_t> text;
        if (!xsflib::ReadFile(set / "expect.txt", text))
        {
            std::printf("FAIL %s: can't read expect.txt\n", xsflib::Utf8(set).c_str());
            unread++;
            continue;
        }

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
                lines.push_back({set, line, SplitFields(line)});
            }

            line.clear();
        }
    }

    return lines;
}

// Plays the track at `path` on the core's playback API, with no front end, for the frames that `frames_at` gives for
// the track's rate. Returns false after printing the player's error if the track doesn't load or start. The render
// stops early only at a change of rate, or after an error.
inline bool PlayOnCore(const std::filesystem::path& path, const std::function<uint64_t(uint32_t rate)>& frames_at,
                       std::vector<int16_t>& samples, uint32_t& rate)
{
    usf::Player player;
    if (!player.Load(xsflib::Utf8(path), xsflib::psf::ReadWholeFile) || !player.Start())
    {
        std::printf("error: %s: %s\n", xsflib::Utf8(path).c_str(), player.Error().c_str());
        return false;
    }

    const auto render = [&player](int16_t* out, size_t count)
    {
        return player.Render(out, count);
    };
    rate = player.SampleRate();
    const size_t frames = static_cast<size_t>(frames_at(rate));
    samples = xsflib::testing::RenderFrames(render, frames, frames);

    return true;
}

// Reads the WAV file at `path`: its samples, with the channels of each frame side by side, and its rate. Returns false
// if the file can't be read or isn't a WAV file.
inline bool ReadWavSamples(const std::filesystem::path& path, std::vector<int16_t>& samples, uint32_t& rate)
{
    std::vector<uint8_t> data;
    if (!xsflib::ReadFile(path, data))
    {
        return false;
    }

    seqlib::WavContents wav;
    try
    {
        wav = seqlib::ReadWav(data);
    }
    catch (const seqlib::ReadError&)
    {
        return false;
    }

    const std::vector<std::vector<int16_t>>& channels = wav.pcm.channels;
    const size_t frames = channels.front().size();
    samples.resize(frames * channels.size());
    for (size_t frame = 0; frame < frames; frame++)
    {
        for (size_t channel = 0; channel < channels.size(); channel++)
        {
            samples[frame * channels.size() + channel] = channels[channel][frame];
        }
    }
    rate = wav.rate;

    return true;
}

} // namespace usf::testing
