// SPDX-License-Identifier: MIT

// The traits that the foobar2000 component and the XMPlay plugin share (xsflib/foobar2000/input.h and
// xsflib/xmplay/input.h): the player, the format's name, the extensions, the limits, the version and the about text.
// The build defines USF_VERSION for both plugins.

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "xsflib/psf/psf.h"

#include "usf/uniform_rate_player.h"
#include "usf/usf_file.h"

namespace usf
{

struct PluginTraits
{
    using Player = UniformRatePlayer;
    using Set = LoadedUsf;

    static constexpr const char* kName = "USF";
    static constexpr std::array<const char*, 2> kExtensions = {"miniusf", "usf"};
    static constexpr uint8_t kPsfVersion = kUsfVersion;
    static constexpr uint64_t kMaxFileSize = 256u << 20; // largest file read into memory

    // The flags set how a set plays. They aren't metadata.
    static constexpr std::array<const char*, 2> kPlaybackTags = {kEnableCompareTag, kEnableFifoFullTag};

    static constexpr const char* kVersion = USF_VERSION;
    static constexpr const char* kAbout =
        "Plays USF (Nintendo 64 music) rips. The game's sound code runs on an emulated Nintendo 64, and the game's "
        "audio microcode runs on the emulated RSP.\n\n"
        "MIT licence. Uses zlib.";

    static std::optional<std::string> LoadSet(const std::string& path, const xsflib::psf::FileReader& reader, Set& out,
                                              std::string_view separators)
    {
        return LoadUsf(path, reader, out, separators);
    }
};

} // namespace usf
