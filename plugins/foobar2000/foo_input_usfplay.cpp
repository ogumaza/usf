// SPDX-License-Identifier: MIT

// foo_input_usfplay: foobar2000 input component for USF (.miniusf and .usf), from xsflib's input template
// (xsflib/foobar2000/input.h). The template's comment says what the component does. This file gives USF's traits: its
// player, names, extensions, GUIDs and limits. plugins/plugin_traits.h contains the traits that the component shares
// with the XMPlay plugin.
//
// The player runs the game's code on an emulated Nintendo 64, and the game's audio microcode on the emulated RSP.
//
// Build: see plugins/foobar2000/README.md (clang-cl on Windows, or Apple Clang on macOS, with the foobar2000 SDK).

#include "xsflib/foobar2000/input.h"
#include <SDK/foobar2000.h>

#include "plugin_traits.h"

namespace
{

struct Traits : usf::PluginTraits
{
    static constexpr const char* kDecoderName = "USF decoder (usfplay)";

    // {9EE95167-9FB2-4A52-9828-70E9A6632A19}
    static constexpr GUID kInputGuid = {0x9ee95167, 0x9fb2, 0x4a52, {0x98, 0x28, 0x70, 0xe9, 0xa6, 0x63, 0x2a, 0x19}};

    // Advanced preferences entries, under Decoding > USF decoder (usfplay).
    static constexpr GUID kBranchGuid = {0xac711efc, 0x1d9b, 0x4cb8, {0xab, 0x06, 0x40, 0x32, 0x72, 0x29, 0xa0, 0x88}};
    static constexpr GUID kLengthGuid = {0x1b3b8d08, 0xabb4, 0x4044, {0xa3, 0x87, 0x73, 0xbd, 0xd1, 0x7d, 0xe9, 0x0d}};
    static constexpr GUID kFadeGuid = {0x28335c77, 0xf3a8, 0x48b3, {0xbe, 0x9f, 0xfb, 0x24, 0x4c, 0x5b, 0x45, 0x5e}};
    static constexpr GUID kEndlessGuid = {0x043ea4c7, 0x26ba, 0x446c, {0x86, 0xc5, 0xad, 0xa3, 0x16, 0x5a, 0x8b, 0x83}};
    static constexpr GUID kRenderAheadGuid = {
        0xe20f707a, 0xab6d, 0x46d9, {0x94, 0xf9, 0x6a, 0xa6, 0x4e, 0x9d, 0x9a, 0x9e}};
    static constexpr GUID kFadeExponentGuid = {
        0xd7926afd, 0x81c1, 0x4a30, {0x88, 0x5a, 0x0b, 0xca, 0x9e, 0x2a, 0x2e, 0x3e}};

    // The settings that shape a track like a track of an album (xsflib/player/settings.h).
    static constexpr GUID kSkipSilenceGuid = {
        0x3ff463bd, 0x437e, 0x4798, {0x83, 0x9a, 0x5b, 0x9d, 0x14, 0xbf, 0xb2, 0x08}};
    static constexpr GUID kStartSilenceGuid = {
        0xe709c400, 0x1979, 0x4d70, {0x9a, 0x35, 0xa6, 0x05, 0x1a, 0xe6, 0xce, 0x78}};
    static constexpr GUID kLoopsGuid = {0xa39ed880, 0xd1d0, 0x4054, {0x9a, 0x83, 0xa5, 0x4a, 0xc9, 0xf1, 0xf2, 0x07}};
    static constexpr GUID kLongLoopGuid = {
        0xb3945dc5, 0x20fa, 0x4ee4, {0x9b, 0x32, 0x5c, 0x4f, 0x8c, 0x20, 0x05, 0x28}};
    static constexpr GUID kLoopFadeGuid = {
        0x8847eabb, 0xe336, 0x4c62, {0xab, 0x12, 0xcd, 0x13, 0x47, 0x24, 0xaa, 0xec}};
    static constexpr GUID kEndSilenceGuid = {
        0x0df5be08, 0x24ee, 0x469b, {0xb7, 0x08, 0x90, 0x50, 0x73, 0x91, 0xfe, 0xa0}};
    static constexpr GUID kIgnoreTagsGuid = {
        0x7cb94cc1, 0xac5b, 0x4a12, {0x8c, 0x61, 0x0d, 0x23, 0x84, 0x90, 0x41, 0xf7}};
    static constexpr GUID kSkipEndGuid = {0xa29c0e22, 0xfd2d, 0x43b0, {0xb1, 0xb6, 0x7a, 0xa1, 0x94, 0x32, 0xbc, 0x49}};
};

xsflib::foobar2000::Settings<Traits> g_settings;
input_singletrack_factory_t<xsflib::foobar2000::Input<Traits>> g_input_usf_factory;

} // namespace

DECLARE_FILE_TYPE("USF files", "*.MINIUSF;*.USF");

DECLARE_COMPONENT_VERSION("USF Decoder (usfplay)", Traits::kVersion, Traits::kAbout);

VALIDATE_COMPONENT_FILENAME("foo_input_usfplay.dll");
