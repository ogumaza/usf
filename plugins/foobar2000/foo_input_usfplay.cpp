// SPDX-License-Identifier: MIT

// foo_input_usfplay: foobar2000 input component for USF (.miniusf and .usf), from xsflib's input template
// (xsflib/foobar2000/input.h). The template's comment says what the component does. This file gives USF's traits: its
// player, names, extensions, GUIDs and limits.
//
// The player runs the game's code on an emulated Nintendo 64, and the game's audio microcode on the emulated RSP.
//
// Build: see plugins/foobar2000/README.md (clang-cl on Windows, or Apple Clang on macOS, with the foobar2000 SDK).

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "xsflib/foobar2000/input.h"
#include "xsflib/psf/psf.h"
#include <SDK/foobar2000.h>

#include "usf/uniform_rate_player.h"
#include "usf/usf_file.h"

namespace
{

struct Traits
{
    using Player = usf::UniformRatePlayer;
    using Set = usf::LoadedUsf;

    static constexpr const char* kName = "USF";
    static constexpr const char* kDecoderName = "USF decoder (usfplay)";
    static constexpr std::array<const char*, 2> kExtensions = {"miniusf", "usf"};
    static constexpr uint8_t kPsfVersion = usf::kUsfVersion;
    static constexpr t_filesize kMaxFileSize = 256u << 20; // largest file read into memory

    // The flags set how a set plays. They aren't metadata. Tag names arrive in lower case.
    static constexpr std::array<const char*, 2> kPlaybackTags = {"_enablecompare", "_enablefifofull"};

    // {9EE95167-9FB2-4A52-9828-70E9A6632A19}
    static constexpr GUID kInputGuid = {0x9ee95167, 0x9fb2, 0x4a52, {0x98, 0x28, 0x70, 0xe9, 0xa6, 0x63, 0x2a, 0x19}};

    // Advanced preferences entries, under Decoding > USF decoder (usfplay).
    static constexpr GUID kBranchGuid = {0xac711efc, 0x1d9b, 0x4cb8, {0xab, 0x06, 0x40, 0x32, 0x72, 0x29, 0xa0, 0x88}};
    static constexpr GUID kLengthGuid = {0x1b3b8d08, 0xabb4, 0x4044, {0xa3, 0x87, 0x73, 0xbd, 0xd1, 0x7d, 0xe9, 0x0d}};
    static constexpr GUID kFadeGuid = {0x28335c77, 0xf3a8, 0x48b3, {0xbe, 0x9f, 0xfb, 0x24, 0x4c, 0x5b, 0x45, 0x5e}};
    static constexpr GUID kEndlessGuid = {0x043ea4c7, 0x26ba, 0x446c, {0x86, 0xc5, 0xad, 0xa3, 0x16, 0x5a, 0x8b, 0x83}};
    static constexpr GUID kRenderAheadGuid = {
        0xe20f707a, 0xab6d, 0x46d9, {0x94, 0xf9, 0x6a, 0xa6, 0x4e, 0x9d, 0x9a, 0x9e}};

    static std::optional<std::string> LoadSet(const std::string& path, const xsflib::psf::FileReader& reader, Set& out,
                                              std::string_view separators)
    {
        return usf::LoadUsf(path, reader, out, separators);
    }
};

xsflib::foobar2000::Settings<Traits> g_settings;
input_singletrack_factory_t<xsflib::foobar2000::Input<Traits>> g_input_usf_factory;

} // namespace

DECLARE_FILE_TYPE("USF files", "*.MINIUSF;*.USF");

DECLARE_COMPONENT_VERSION("USF Decoder (usfplay)", USF_VERSION,
                          "Plays USF (Nintendo 64 music) rips. The game's sound code runs on an emulated Nintendo 64, "
                          "and the game's audio microcode runs on the emulated RSP.\n\n"
                          "MIT licence. Uses zlib.");

VALIDATE_COMPONENT_FILENAME("foo_input_usfplay.dll");
