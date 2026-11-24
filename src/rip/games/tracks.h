// SPDX-License-Identifier: MIT

// What the profiles' tracks share: titles made from a song's number, and the lengths that runs of the songs measured.

#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "rip/profile.h"

namespace usf::rip::games
{

// The length that runs of a song measured, by the song's number.
struct SongLength
{
    uint32_t number = 0;
    double seconds = 0.0;
};

// The fade of a track whose length was measured: the fade of a loop.
inline constexpr double kMeasuredFadeSeconds = 7.0;

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

// Adds ", version N" to the title of each track whose title an earlier track has. N counts the tracks with that title,
// from 1 for the first. Several tracks of one song, each in a state that the game's code sets, then get distinct
// titles.
void AddVersionTitles(std::vector<Track>& tracks);

} // namespace usf::rip::games
