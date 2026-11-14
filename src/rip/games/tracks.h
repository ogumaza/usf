// SPDX-License-Identifier: MIT

// What the profiles' tracks share: titles made from a number and a song's name, and the lengths that runs of the songs
// measured.

#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "rip/profile.h"
#include "ultralib/n64/song_names.h"

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

// A song name, by the song number. The tables in ultralib/n64/song_names.h name the songs of some games.
using ultralib::n64::SongName;

// The name of song `number` in `names`, or null.
const char* FindSongName(std::span<const SongName> names, uint32_t number);

// The name of song `number` in `set_names`, or else in `names`, or null. A profile lists in `set_names` the names that
// its titles word differently from ultralib's table.
const char* FindSongName(std::span<const SongName> set_names, std::span<const SongName> names, uint32_t number);

// The title of song `number`: NumberTitle(label, number, base), then `name` and `detail`, each after a separator, such
// as "Sequence 0x2E: Ganon's Tower, entrance". Without a name, `detail` takes its place.
std::string SongTitle(std::string_view label, uint32_t number, TitleBase base, const char* name,
                      const char* detail = nullptr);

// Makes the track `number` the way NumberedTrack does. Its title is SongTitle with its name in `names`.
Track NamedOrNumberedTrack(uint32_t number, std::span<const SongName> names, std::string_view label, TitleBase base,
                           std::span<const SongLength> lengths = {});

} // namespace usf::rip::games
