// SPDX-License-Identifier: MIT

// The ROM sets of arcade boards, through xsflib's loader. The user gives a set as a ZIP archive or as a folder of its
// chips. The ripper finds each chip that a profile names by its SHA-1 and size, whatever the file is called, and puts
// the chips together into the image that the board's CPU sees.

#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "xsflib/rip/rom_set.h"

#include "rip/profile.h"

namespace usf::rip
{

// A file of a set.
using SetFile = xsflib::rip::SetFile;

// Whether `path` may be a ROM set: a folder or a ZIP archive.
using xsflib::rip::IsRomSet;

// Reads the files of the set at `path`: the entries of a ZIP archive, or the files of a folder (not of its subfolders)
// whose sizes some profile's chip has. Returns why it can't, or nothing.
std::optional<std::string> ReadRomSet(const std::filesystem::path& path, std::span<const GameProfile> profiles,
                                      std::vector<SetFile>& files);

struct RomSetImage
{
    const GameProfile* profile = nullptr;   // The game whose chips are all in the set, or null.
    std::vector<uint8_t> rom;               // Its image, in console byte order.
    std::string problem;                    // Without a game: why.
    std::vector<const GameProfile*> others; // With a game: the first profile of each other game that the set contains.
};

// Puts together the image of the first profile whose chips are all among `files`. A profile with another image's SHA-1
// is another game.
RomSetImage BuildRomSetImage(std::span<const GameProfile> profiles, const std::vector<SetFile>& files);

} // namespace usf::rip
