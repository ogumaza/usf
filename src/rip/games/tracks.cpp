// SPDX-License-Identifier: MIT

#include "rip/games/tracks.h"

#include <cstdio>
#include <initializer_list>
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

const char* FindSongName(std::span<const SongName> names, uint32_t number)
{
    for (const SongName& song : names)
    {
        if (song.index == number)
        {
            return song.name;
        }
    }

    return nullptr;
}

const char* FindSongName(std::span<const SongName> set_names, std::span<const SongName> names, uint32_t number)
{
    const char* name = FindSongName(set_names, number);

    return name != nullptr ? name : FindSongName(names, number);
}

std::string SongTitle(std::string_view label, uint32_t number, TitleBase base, const char* name, const char* detail)
{
    std::string title = NumberTitle(label, number, base);
    const char* separator = ": ";
    for (const char* part : {name, detail})
    {
        if (part != nullptr)
        {
            title += separator;
            title += part;
            separator = ", ";
        }
    }

    return title;
}

Track NamedOrNumberedTrack(uint32_t number, std::span<const SongName> names, std::string_view label, TitleBase base,
                           std::span<const SongLength> lengths)
{
    return NumberedTrack(number, SongTitle(label, number, base, FindSongName(names, number)), lengths);
}

} // namespace usf::rip::games
