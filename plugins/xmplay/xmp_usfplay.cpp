// SPDX-License-Identifier: MIT

// xmp-usfplay: XMPlay input plugin for USF (.miniusf and .usf), from xsflib's plugin template (xsflib/xmplay/input.h).
// The template's comment says what the plugin does. This file gives USF's traits: its player, names, extensions and
// limits.
//
// The player runs the game's code on an emulated Nintendo 64, and the game's audio microcode on the emulated RSP.
//
// Build: see plugins/xmplay/README.md (clang-cl for 32-bit Windows, with the XMPlay plugin SDK).

#include <windows.h>
#include <xmpin.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "xsflib/psf/psf.h"
#include "xsflib/xmplay/input.h"

#include "usf/uniform_rate_player.h"
#include "usf/usf_file.h"

namespace
{

struct Traits
{
    using Player = usf::UniformRatePlayer;
    using Set = usf::LoadedUsf;

    static constexpr const char* kName = "USF";
    static constexpr std::array<const char*, 2> kExtensions = {"miniusf", "usf"};
    static constexpr uint8_t kPsfVersion = usf::kUsfVersion;
    static constexpr uint64_t kMaxFileSize = 256u << 20; // largest file read into memory

    // The flags set how a set plays. They aren't metadata. Tag names arrive in lower case.
    static constexpr std::array<const char*, 2> kPlaybackTags = {"_enablecompare", "_enablefifofull"};

    static constexpr const char* kVersion = USF_VERSION;
    static constexpr const char* kAbout =
        "Plays USF (Nintendo 64 music) rips. The game's sound code runs on an emulated Nintendo 64, and the game's "
        "audio microcode runs on the emulated RSP.\n\n"
        "MIT licence. Uses zlib.";

    static std::optional<std::string> LoadSet(const std::string& path, const xsflib::psf::FileReader& reader, Set& out,
                                              std::string_view separators)
    {
        return usf::LoadUsf(path, reader, out, separators);
    }
};

} // namespace

// XMPlay looks the plugin up by this name (the SDK's xmpin.def exports it).
XMPIN* WINAPI XMPIN_GetInterface(DWORD face, InterfaceProc faceproc)
{
    return xsflib::xmplay::Plugin<Traits>::GetInterface(face, faceproc);
}
