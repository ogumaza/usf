// SPDX-License-Identifier: MIT

#include "rip/games/tracks.h"

#include <cstdio>
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

Track NamedOrNumberedTrack(uint32_t number, std::span<const NamedNumber> names, std::string_view label, TitleBase base,
                           std::span<const SongLength> lengths)
{
    for (const NamedNumber& named : names)
    {
        if (named.number == number)
        {
            return NumberedTrack(number, named.name, lengths);
        }
    }

    return NumberedTrack(number, NumberTitle(label, number, base), lengths);
}

} // namespace usf::rip::games
