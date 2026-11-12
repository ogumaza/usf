# SPDX-License-Identifier: MIT

# foo_input_usfplay: the foobar2000 component. It's a DLL built with clang-cl on Windows. On macOS it's a .component
# bundle. The top-level CMakeLists.txt adds it when FOOBAR2000_SDK_DIR is set (see README.md). xsflib's
# xsflib_add_foobar2000_component compiles the parts of the SDK that a component requires.

# On macOS, the SDK's shared part includes Objective-C++ files. CMake enables a language only at file scope.
if (APPLE)
    enable_language(OBJCXX)
endif()

xsflib_add_foobar2000_component(foo_input_usfplay
    SDK ${FOOBAR2000_SDK_DIR}
    SOURCES foo_input_usfplay.cpp
    LIBRARIES usf_player
    VERSION ${usf_VERSION}
    BUNDLE_ID com.foobar2000.foo-input-usfplay
    OPTIONS ${USF_WARNINGS})
xsflib_add_version(foo_input_usfplay VERSION ${usf_VERSION} DEFINE USF_VERSION DESCRIPTION "USF decoder for foobar2000"
                   PRODUCT usf COPYRIGHT "Copyright (c) 2026 the usf authors. MIT licence.")

# plugins/plugin_traits.h contains the traits that foo_input_usfplay shares with the XMPlay plugin.
target_include_directories(foo_input_usfplay PRIVATE ${PROJECT_SOURCE_DIR}/plugins)
