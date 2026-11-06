// SPDX-License-Identifier: MIT

#include "rip/trim.h"

#include <algorithm>
#include <bit>

namespace usf::rip
{

namespace
{

// Chunks closer than this are joined: a chunk's length and offset take 8 bytes.
constexpr uint32_t kMaxGap = 8;

// A ROM chunk in LE word order from the ROM in console order. Bytes past the ROM's end read as zero.
Chunk RomChunk(const std::vector<uint8_t>& rom, uint32_t offset, uint32_t length)
{
    Chunk chunk;
    chunk.offset = offset;
    chunk.data.resize(length);
    for (uint32_t i = 0; i < length; i++)
    {
        const uint32_t source = (offset + i) ^ 3;
        chunk.data[i] = source < rom.size() ? rom[source] : 0;
    }

    return chunk;
}

Chunk StateChunk(const std::vector<uint8_t>& state, uint32_t offset, uint32_t length)
{
    Chunk chunk;
    chunk.offset = offset;
    chunk.data.assign(state.begin() + offset, state.begin() + offset + length);

    return chunk;
}

std::vector<Chunk> RomChunks(const std::vector<uint8_t>& rom, const WordSet& words)
{
    std::vector<Chunk> chunks;
    for (const n64::ByteRange& range : words.Ranges(kMaxGap))
    {
        chunks.push_back(RomChunk(rom, range.offset, range.length));
    }

    return chunks;
}

} // namespace

WordSet::WordSet(uint32_t bytes) : bits_((bytes / 4 + 63) / 64), words_(bytes / 4)
{
}

void WordSet::Add(uint32_t offset, uint32_t length)
{
    if (length == 0 || words_ == 0)
    {
        return;
    }

    const uint32_t first = offset / 4;
    const auto last = static_cast<uint32_t>(std::min<uint64_t>((uint64_t{offset} + length - 1) / 4, words_ - 1));
    for (uint32_t word = first; word <= last; word++)
    {
        bits_[word / 64] |= uint64_t{1} << (word % 64);
    }
}

bool WordSet::Has(uint32_t word) const
{
    return word < words_ && (bits_[word / 64] & (uint64_t{1} << (word % 64))) != 0;
}

uint64_t WordSet::Count() const
{
    uint64_t count = 0;
    for (uint64_t bits : bits_)
    {
        count += static_cast<uint64_t>(std::popcount(bits));
    }

    return count;
}

uint32_t WordSet::Words() const
{
    return words_;
}

void WordSet::Intersect(const WordSet& other)
{
    for (size_t i = 0; i < bits_.size(); i++)
    {
        bits_[i] &= i < other.bits_.size() ? other.bits_[i] : 0;
    }
}

void WordSet::Subtract(const WordSet& other)
{
    for (size_t i = 0; i < bits_.size() && i < other.bits_.size(); i++)
    {
        bits_[i] &= ~other.bits_[i];
    }
}

std::vector<n64::ByteRange> WordSet::Ranges(uint32_t max_gap) const
{
    std::vector<n64::ByteRange> ranges;
    uint32_t word = 0;
    while (word < words_)
    {
        if (!Has(word))
        {
            word++;
            continue;
        }

        const uint32_t start = word;
        while (word < words_ && Has(word))
        {
            word++;
        }

        const uint32_t offset = start * 4;
        const uint32_t end = word * 4;
        if (!ranges.empty() && offset - (ranges.back().offset + ranges.back().length) <= max_gap)
        {
            ranges.back().length = end - ranges.back().offset;
        }
        else
        {
            ranges.push_back({offset, end - offset});
        }
    }

    return ranges;
}

uint32_t RomImageSize(uint64_t rom_size)
{
    uint32_t size = 0x100000;
    while (size < rom_size && size < 0x10000000)
    {
        size *= 2;
    }

    return size;
}

TrackWords WordsOf(const n64::Coverage& coverage, uint32_t rom_image_size, uint32_t rdram_size)
{
    TrackWords words{WordSet(rom_image_size), WordSet(rdram_size)};
    for (const n64::ByteRange& range : coverage.RomRanges())
    {
        words.rom.Add(range.offset, range.length);
    }

    for (const n64::ByteRange& range : coverage.RdramRanges())
    {
        words.rdram.Add(range.offset, range.length);
    }

    return words;
}

SetSections BuildSet(const Snapshot& snapshot, const std::vector<Track>& tracks, const std::vector<TrackWords>* words)
{
    // The library holds the whole state, RDRAM included. The legacy players' timing can reach RDRAM words that the
    // core's runs never read.
    SetSections set;
    set.library.state = std::vector<Chunk>{StateChunk(snapshot.state, 0, static_cast<uint32_t>(snapshot.state.size()))};
    if (words == nullptr)
    {
        set.library.rom.emplace();
        const auto rom_words = static_cast<uint32_t>((snapshot.rom.size() + 3) & ~size_t{3});
        if (rom_words != 0)
        {
            set.library.rom->push_back(RomChunk(snapshot.rom, 0, rom_words));
        }
    }
    else
    {
        // The ROM words that every track read go in the library, and the rest of each track's in its mini.
        WordSet common = words->front().rom;
        for (const TrackWords& track : *words)
        {
            common.Intersect(track.rom);
        }
        set.library.rom = RomChunks(snapshot.rom, common);

        for (const TrackWords& track : *words)
        {
            WordSet rest = track.rom;
            rest.Subtract(common);
            Sections mini;
            if (rest.Count() != 0)
            {
                mini.rom = RomChunks(snapshot.rom, rest);
            }
            set.minis.push_back(std::move(mini));
        }
    }

    // Each mini writes its number into the selector, and its patches.
    set.minis.resize(tracks.size());
    for (size_t i = 0; i < tracks.size(); i++)
    {
        const std::vector<uint8_t> track_state = TrackState(snapshot, tracks[i]);
        std::vector<Chunk> chunks{StateChunk(track_state, snapshot.selector_offset, snapshot.selector_size)};
        for (const RdramPatch& patch : tracks[i].rdram_patches)
        {
            chunks.push_back(StateChunk(track_state, RdramStateOffset(patch.address),
                                        static_cast<uint32_t>(4 * patch.words.size())));
        }
        set.minis[i].state = std::move(chunks);
    }

    return set;
}

} // namespace usf::rip
