// SPDX-License-Identifier: MIT

// xmp-usfplay: XMPlay input plugin for USF (.miniusf and .usf), from xsflib's plugin template (xsflib/xmplay/input.h).
// The template's comment says what the plugin does. This file passes USF's traits to the template: its player, names,
// extensions and limits. plugins/plugin_traits.h contains them. The foobar2000 component uses them too.
//
// The player runs the game's code on an emulated Nintendo 64, and the game's audio microcode on the emulated RSP.
//
// Build: see plugins/xmplay/README.md (clang-cl for 32-bit Windows, with the XMPlay plugin SDK).

#include <windows.h>
#include <xmpin.h>

#include "xsflib/xmplay/input.h"

#include "plugin_traits.h"

// XMPlay looks the plugin up by this name (the SDK's xmpin.def exports it).
XMPIN* WINAPI XMPIN_GetInterface(DWORD face, InterfaceProc faceproc)
{
    return xsflib::xmplay::Plugin<usf::PluginTraits>::GetInterface(face, faceproc);
}
