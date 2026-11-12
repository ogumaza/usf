// SPDX-License-Identifier: MIT

#include "rip/trim.h"

#include <utility>

namespace usf::rip
{
namespace
{

// Chunks at most this far apart are joined: a chunk's length and offset take 8 bytes.
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

TrackWords WordsOf(const n64::Coverage& coverage, uint32_t rom_image_size, uint32_t rdram_size)
{
    TrackWords words{WordSet(rom_image_size), WordSet(rdram_size)};
    words.rom.Merge(coverage.RomWords());
    words.rdram.Merge(coverage.RdramWords());

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
        const auto rom_bytes = static_cast<uint32_t>((snapshot.rom.size() + 3) & ~size_t{3});
        if (rom_bytes != 0)
        {
            set.library.rom->push_back(RomChunk(snapshot.rom, 0, rom_bytes));
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
