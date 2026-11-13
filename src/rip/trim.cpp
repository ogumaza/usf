// SPDX-License-Identifier: MIT

#include "rip/trim.h"

#include <utility>

#include "n64/save_state.h"

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

// The state without the words of RDRAM that are zero. The header and the RSP memory stay whole.
std::vector<Chunk> StateChunksWithoutZeroWords(const std::vector<uint8_t>& state)
{
    const auto rdram_size = static_cast<uint32_t>(state.size() - kStateOverhead);
    const uint8_t* rdram = state.data() + n64::kStateRdramOffset;
    WordSet nonzero(rdram_size);
    for (uint32_t word = 0; word < rdram_size / 4; word++)
    {
        const uint8_t* bytes = rdram + size_t{4} * word;
        if ((bytes[0] | bytes[1] | bytes[2] | bytes[3]) != 0)
        {
            nonzero.AddWord(word);
        }
    }

    std::vector<Chunk> chunks{StateChunk(state, 0, n64::kStateRdramOffset)};
    for (const n64::ByteRange& range : nonzero.Ranges(kMaxGap))
    {
        chunks.push_back(StateChunk(state, n64::kStateRdramOffset + range.offset, range.length));
    }

    chunks.push_back(StateChunk(state, n64::kStateRdramOffset + rdram_size, kStateOverhead - n64::kStateRdramOffset));

    return chunks;
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
    SetSections set;
    if (words == nullptr)
    {
        set.library.state =
            std::vector<Chunk>{StateChunk(snapshot.state, 0, static_cast<uint32_t>(snapshot.state.size()))};
        set.library.rom.emplace();
        const auto rom_bytes = static_cast<uint32_t>((snapshot.rom.size() + 3) & ~size_t{3});
        if (rom_bytes != 0)
        {
            set.library.rom->push_back(RomChunk(snapshot.rom, 0, rom_bytes));
        }
    }
    else
    {
        // The library holds the state's value for every word of RDRAM, whether a run read it or not. The legacy
        // players' timing can reach RDRAM words that the core's runs never read. A word that is zero takes no chunk: a
        // player fills the state image with zeros before it loads the library.
        set.library.state = StateChunksWithoutZeroWords(snapshot.state);

        // Every ROM word that a track read goes in the library. A mini holds only its selector and its patches.
        WordSet read = words->front().rom;
        for (const TrackWords& track : *words)
        {
            read.Merge(track.rom);
        }
        set.library.rom = RomChunks(snapshot.rom, read);
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
