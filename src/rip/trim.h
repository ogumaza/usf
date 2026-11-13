// SPDX-License-Identifier: MIT

// Trimming by coverage: a set keeps the ROM words that a track's run read. The library keeps the state and every ROM
// word that a track read. Each mini keeps only its selector and its patches.
//
// RDRAM isn't trimmed by coverage. The legacy players' timing differs from the core's. In a legacy player, a game's
// code can take paths that the core's runs never take. Those paths can read RDRAM words that the core's runs never
// read. Such words would read as zero in a set trimmed that way. The library leaves out only the words of RDRAM that
// are zero in the state. A player fills the state image with zeros before it loads a set, so those words read the same.

#pragma once

#include <cstdint>
#include <vector>

#include "n64/coverage.h"
#include "n64/word_set.h"
#include "rip/snapshot.h"
#include "usf/usf_file.h"

namespace usf::rip
{

// Trimming uses the word sets of the core.
using WordSet = n64::WordSet;

// What one track's run read.
struct TrackWords
{
    WordSet rom;
    WordSet rdram;
};

// The words that `coverage` recorded, for a ROM image of `rom_image_size` bytes and RDRAM of `rdram_size` bytes.
TrackWords WordsOf(const n64::Coverage& coverage, uint32_t rom_image_size, uint32_t rdram_size);

// A set's files before their tags: the library's sections and each mini's.
struct SetSections
{
    Sections library;
    std::vector<Sections> minis;
};

// The sections of a set of `tracks`. With `words`, one entry for each track, the ROM is trimmed, and the library's
// state leaves out the words of RDRAM that are zero. Without it, the library holds the whole ROM and the whole state.
SetSections BuildSet(const Snapshot& snapshot, const std::vector<Track>& tracks, const std::vector<TrackWords>* words);

} // namespace usf::rip
