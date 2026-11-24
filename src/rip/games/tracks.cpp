// SPDX-License-Identifier: MIT

#include "rip/games/tracks.h"

#include <cstdio>
#include <map>
#include <string>
#include <utility>

namespace usf::rip::games
{

std::string NumberTitle(std::string_view label, uint32_t number, TitleBase base)
{
    char digits[16];
    std::snprintf(digits, sizeof(digits), base == TitleBase::kHex ? "0x%02X" : "%u", static_cast<unsigned>(number));

    return std::string(label) + " " + digits;
}

Track NumberedTrack(uint32_t number, std::string name, std::span<const SongLength> lengths)
{
    Track track;
    track.number = number;
    track.name = std::move(name);
    for (const SongLength& length : lengths)
    {
        if (length.number == number)
        {
            track.length_seconds = length.seconds;
            track.fade_seconds = kMeasuredFadeSeconds;
        }
    }

    return track;
}

void AddVersionTitles(std::vector<Track>& tracks)
{
    std::map<std::string, int> seen;
    for (Track& track : tracks)
    {
        const int version = ++seen[track.name];
        if (version > 1)
        {
            track.name += ", version " + std::to_string(version);
        }
    }
}

} // namespace usf::rip::games
