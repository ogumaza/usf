// SPDX-License-Identifier: MIT

// USF files: the PSF container with version 0x21 and an empty program, a reserved area of two sections, and tags.
// WriteUsf makes a file. LoadUsf loads a track with its libraries into a ROM image and a state image, as a player does
// before it starts the emulator.

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "xsflib/psf/psf.h"

#include "n64/constants.h"
#include "n64/save_state.h"

namespace usf
{

inline constexpr uint8_t kUsfVersion = 0x21;

// The state image is this much larger than the RDRAM that it holds: the header comes before the RDRAM, and DMEM and
// IMEM come after it.
inline constexpr uint32_t kStateOverhead = n64::kStateOverhead;

// The cartridge offsets that a ROM chunk may cover: cartridge space runs from 0x10000000 up to the PIF's ROM at
// 0x1FC00000.
inline constexpr uint32_t kMaxRomOffset = n64::kPifRom - n64::kCartRom;

// One chunk of a section: data for the image at `offset`. In the ROM section, offsets are cartridge offsets and the
// data are in LE word order. In the state section, offsets and data are those of the state image.
struct Chunk
{
    uint32_t offset = 0;
    std::vector<uint8_t> data;
};

// The reserved area of one file. A section that is absent holds nothing. An empty section is present without chunks.
struct Sections
{
    std::optional<std::vector<Chunk>> rom;
    std::optional<std::vector<Chunk>> state;
};

// The reserved area for `sections`: each section's marker (SR64, or 4 zero bytes for an absent one), then its chunks
// and a zero length.
std::vector<uint8_t> WriteReservedArea(const Sections& sections);

// Parses a reserved area. Returns the error for one that ends inside a field or a chunk's data, or nothing.
std::optional<std::string> ParseReservedArea(std::span<const uint8_t> reserved, Sections& out);

// A whole file: the header with version 0x21, no program and a CRC of 0, the reserved area and the tags. In usf_write.
std::vector<uint8_t> WriteUsf(const Sections& sections, const xsflib::psf::Tags& tags);

// The tag names of the two flags in parsed tags. The parser stores tag names in lower case. The format spells the
// second one _enableFIFOfull.
inline constexpr const char* kEnableCompareTag = "_enablecompare";
inline constexpr const char* kEnableFifoFullTag = "_enablefifofull";

// A track loaded with its libraries.
struct LoadedUsf
{
    // The ROM in console byte order, up to the highest byte that a chunk wrote. A player's ROM image is the smallest
    // power of two, at least 1 MiB, that covers it.
    std::vector<uint8_t> rom;

    // The state image: a Project64 1.4 save state, 0x275C bytes plus the RDRAM size.
    std::vector<uint8_t> state;

    // The two flags, from any file of the chain.
    bool enable_compare = false;
    bool enable_fifo_full = false;

    // The tags of the file that was opened.
    xsflib::psf::Tags tags;

    // The files read, in the order they were applied.
    std::vector<std::string> sources;
};

// Loads the track at `path`: its `_lib` first, by the same steps and relative to its folder, then its reserved area,
// then `_lib2`, `_lib3` and on. A `_lib` tag with an empty value names no library. Returns the error, or nothing. It
// rejects a file that isn't a USF file or contains a program, a reserved area that ends inside a field or a chunk, a
// ROM chunk past the end of cartridge space, a state chunk outside the state image, a chain deeper than xsflib's limit,
// a set of more files than xsflib's limit, and a set without a state or with a state whose identifier or RDRAM size is
// wrong. `separators` are the characters that end a path's folder. A file in an archive may have others.
std::optional<std::string> LoadUsf(const std::string& path, const xsflib::psf::FileReader& reader, LoadedUsf& out,
                                   std::string_view separators = "/\\");

} // namespace usf
