# SPDX-License-Identifier: MIT

# xmp-usfplay: the XMPlay input plugin, a 32-bit DLL built with clang-cl. XMPlay is a 32-bit program. Added by the
# top-level CMakeLists.txt when XMPLAY_SDK_DIR is set. See README.md. xsflib builds it against the SDK's headers
# (xsflib_add_xmplay_plugin).

xsflib_add_xmplay_plugin(xmp-usfplay
    SDK ${XMPLAY_SDK_DIR}
    SOURCES xmp_usfplay.cpp
    LIBRARIES usf_player
    OPTIONS ${USF_WARNINGS})
xsflib_add_version(xmp-usfplay VERSION ${usf_VERSION} DEFINE USF_VERSION DESCRIPTION "USF plugin for XMPlay"
                   PRODUCT usf COPYRIGHT "Copyright (c) 2026 the usf authors. MIT licence.")

# plugins/plugin_traits.h contains the traits that xmp-usfplay shares with the foobar2000 component.
target_include_directories(xmp-usfplay PRIVATE ${PROJECT_SOURCE_DIR}/plugins)
