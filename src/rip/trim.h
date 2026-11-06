// SPDX-License-Identifier: MIT

// Trimming by coverage: a set keeps the ROM words that a track's run read. The library keeps the whole state and the
// ROM words that every track read. Each mini keeps its other ROM words and its selector.
//
// RDRAM isn't trimmed. The legacy players' timing differs from the core's. In a legacy player, a game's code can take
// paths that the core's runs never take. Those paths can read RDRAM words that the core's runs never read. Such words
// would read as zero in a trimmed set.

#pragma once

#include <cstdint>
#include <vector>

#include "n64/coverage.h"
#include "rip/snapshot.h"
#include "usf/usf_file.h"

namespace usf::rip
{

// A set of the 4-byte words of an image.
class WordSet
{
public:
    WordSet() = default;
    explicit WordSet(uint32_t bytes);

    // Adds the words that bytes [offset, offset + length) touch, up to the set's size.
    void Add(uint32_t offset, uint32_t length);
    bool Has(uint32_t word) const;
    uint64_t Count() const;
    uint32_t Words() const;

    // Keeps only the words that are also in `other`, or only those that aren't.
    void Intersect(const WordSet& other);
    void Subtract(const WordSet& other);

    // The words as byte ranges in ascending order. Ranges less than `max_gap` bytes apart are joined, with the words
    // between them.
    std::vector<n64::ByteRange> Ranges(uint32_t max_gap = 0) const;

private:
    std::vector<uint64_t> bits_;
    uint32_t words_ = 0;
};

// What one track's run read.
struct TrackWords
{
    WordSet rom;
    WordSet rdram;
};

// The ROM image's size for a ROM of `rom_size` bytes: the smallest power of two, at least 1 MiB, that covers it.
uint32_t RomImageSize(uint64_t rom_size);

// The words that `coverage` recorded, for a ROM image of `rom_image_size` bytes and RDRAM of `rdram_size` bytes.
TrackWords WordsOf(const n64::Coverage& coverage, uint32_t rom_image_size, uint32_t rdram_size);

// A set's files before their tags: the library's sections and each mini's.
struct SetSections
{
    Sections library;
    std::vector<Sections> minis;
};

// The sections of a set of `tracks`. The library holds the whole state. With `words`, one entry for each track, the
// ROM is trimmed. Without it, the library holds the whole ROM.
SetSections BuildSet(const Snapshot& snapshot, const std::vector<Track>& tracks, const std::vector<TrackWords>* words);

} // namespace usf::rip
