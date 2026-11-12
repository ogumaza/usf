// SPDX-License-Identifier: MIT

// What the profiles' tracks share: titles made from a number, and the lengths that runs of the songs measured.

#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "rip/profile.h"

namespace usf::rip::games
{

// The length that runs of a song measured, by the song's number.
struct SongLength
{
    uint32_t number = 0;
    double seconds = 0.0;
};

// The fade of a track whose length was measured.
inline constexpr double kMeasuredFadeSeconds = 10.0;

// How a title writes its number.
enum class TitleBase
{
    kDecimal, // "Song 5"
    kHex,     // "Song 0x05", with at least two digits
};

// A title of `label` and `number`, such as "Song 5" or "Song 0x05".
std::string NumberTitle(std::string_view label, uint32_t number, TitleBase base);

// The track `number`, titled `name`. When `lengths` has the number, the track has that length and a fade of
// kMeasuredFadeSeconds.
Track NumberedTrack(uint32_t number, std::string name, std::span<const SongLength> lengths = {});

// The title of a track that a profile names, by the track's number.
struct NamedNumber
{
    uint32_t number = 0;
    const char* name = nullptr;
};

// Makes the track `number` the way NumberedTrack does. Its title is its name in `names`, or else NumberTitle(label,
// number, base).
Track NamedOrNumberedTrack(uint32_t number, std::span<const NamedNumber> names, std::string_view label, TitleBase base,
                           std::span<const SongLength> lengths = {});

} // namespace usf::rip::games
