# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.20)
project(usf VERSION 1.1.1 LANGUAGES C CXX)

# xsflib is a library for players and rippers of the PSF family. It's a submodule. Its XsflibProject.cmake sets what a
# project built on it starts with: C++20, a release build by default, the static C runtime, the options that keep the
# output the same on every platform, and the warnings for usf's targets (USF_WARNINGS). It refuses the compilers whose
# output would differ from other builds.
if (NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/externals/xsflib/CMakeLists.txt")
    message(FATAL_ERROR "externals/xsflib is missing. Clone with git clone --recursive, or run "
                        "git submodule update --init --recursive in a clone.")
endif()
include(externals/xsflib/cmake/XsflibProject.cmake)
xsflib_project(CHECK_COMPILERS NAME usf PREFIX USF)

# clang-cl warns of designated initializers that leave fields out, as the game profiles do. Filling in every field would
# copy non-zero defaults into each initializer, where they can drift.
if (USF_CLANG_CL)
    list(APPEND USF_WARNINGS -Wno-missing-designated-field-initializers)
endif()

# The game profiles have text outside ASCII. MSVC reads source files as UTF-8 only with /utf-8.
if (MSVC)
    add_compile_options(/utf-8)
endif()

# The player's programs use xsflib's playback framework and its command-line frame. The framework requires zlib. The
# frame's FLAC output requires libFLAC.
set(XSFLIB_PSF ON)
set(XSFLIB_CLI ON)
xsflib_add_libraries(xsflib)

# The emulator core: the VR4300, the RSP, the RCP's interfaces, the PIF, boot, save states and coverage.
add_library(usf_n64 STATIC
    src/n64/aleck64.cpp
    src/n64/bits.h
    src/n64/boot.cpp
    src/n64/constants.h
    src/n64/coverage.cpp
    src/n64/coverage.h
    src/n64/cpu.cpp
    src/n64/cpu.h
    src/n64/cpu_cop0.cpp
    src/n64/cpu_cop0.h
    src/n64/cpu_fast.cpp
    src/n64/cpu_fpu.cpp
    src/n64/mips_registers.h
    src/n64/pif.cpp
    src/n64/pif.h
    src/n64/rcp.cpp
    src/n64/rdram.cpp
    src/n64/rsp.cpp
    src/n64/rsp.h
    src/n64/rsp_vector.cpp
    src/n64/rsp_vector.h
    src/n64/save_state.cpp
    src/n64/save_state.h
    src/n64/system.cpp
    src/n64/system.h
    src/n64/word_set.cpp
    src/n64/word_set.h
)
target_include_directories(usf_n64 PUBLIC src)
target_link_libraries(usf_n64 PUBLIC xsflib::common)
target_compile_options(usf_n64 PRIVATE ${USF_WARNINGS})

# The FPU's arithmetic runs on the host's floating point in the rounding mode that the emulated FCR31 selects, and
# reads the host's exception flags. The compiler must keep each operation between the calls that set the mode and
# read the flags.
if (MSVC)
    set_source_files_properties(src/n64/cpu_fpu.cpp PROPERTIES COMPILE_OPTIONS "/clang:-ffp-model=strict")
else()
    set_source_files_properties(src/n64/cpu_fpu.cpp PROPERTIES COMPILE_OPTIONS "-ffp-model=strict")
endif()

# The USF format: its files, the loading of a track with its libraries, and the start of a track on the core.
add_library(usf_format STATIC
    src/usf/player.cpp
    src/usf/player.h
    src/usf/track.cpp
    src/usf/track.h
    src/usf/usf_file.cpp
    src/usf/usf_file.h
)
target_include_directories(usf_format PUBLIC src)
target_link_libraries(usf_format PUBLIC usf_n64 xsflib::psf_container)
target_compile_options(usf_format PRIVATE ${USF_WARNINGS})

# The writer of USF files, on xsflib's PSF writer. That writer requires zlib. usf_format and n64run leave it out.
add_library(usf_write STATIC src/usf/usf_write.cpp)
target_link_libraries(usf_write PUBLIC usf_format xsflib::psf)
target_compile_options(usf_write PRIVATE ${USF_WARNINGS})

# The ripper: the game profiles, the snapshot, the track player, the length analysis, trimming by coverage, the set's
# files and the generic driver with its MIPS encoder.
add_library(usf_rip STATIC
    src/rip/driver.cpp
    src/rip/driver.h
    src/rip/games/aleck64.cpp
    src/rip/games/aleck64.h
    src/rip/games/aleck64_eleven_beat.cpp
    src/rip/games/aleck64_hanabi_de_doon.cpp
    src/rip/games/aleck64_hi_pai_paradise.cpp
    src/rip/games/aleck64_hi_pai_paradise_2.cpp
    src/rip/games/aleck64_kurukuru_fever.cpp
    src/rip/games/aleck64_magical_tetris_challenge.cpp
    src/rip/games/aleck64_mayjinsen_3.cpp
    src/rip/games/aleck64_star_soldier.cpp
    src/rip/games/aleck64_super_real_mahjong_vs.cpp
    src/rip/games/aleck64_tower_and_shaft.cpp
    src/rip/games/aleck64_vivid_dolls.cpp
    src/rip/games/games.h
    src/rip/games/nus_czle_usa_rev2.cpp
    src/rip/games/nus_n2ve_usa.cpp
    src/rip/games/nus_ncte_usa_rev1.cpp
    src/rip/games/nus_ncwe_usa.cpp
    src/rip/games/nus_ngee_usa.cpp
    src/rip/games/nus_nkte_usa.cpp
    src/rip/games/nus_nm8e_usa.cpp
    src/rip/games/nus_nmqe_usa.cpp
    src/rip/games/nus_np3e_usa.cpp
    src/rip/games/nus_npfe_usa.cpp
    src/rip/games/nus_npge_usa.cpp
    src/rip/games/nus_npne_usa.cpp
    src/rip/games/nus_npoe_usa_rev2.cpp
    src/rip/games/nus_nsme_usa.cpp
    src/rip/games/nus_nsme_usa_music.cpp
    src/rip/games/nus_nwre_usa_rev1.cpp
    src/rip/games/nus_nyse_usa.cpp
    src/rip/games/nus_nzse_usa.cpp
    src/rip/games/tracks.cpp
    src/rip/games/tracks.h
    src/rip/length.cpp
    src/rip/length.h
    src/rip/mips.cpp
    src/rip/mips.h
    src/rip/player.cpp
    src/rip/player.h
    src/rip/profile.h
    src/rip/profiles.cpp
    src/rip/profiles.h
    src/rip/rip.cpp
    src/rip/rip.h
    src/rip/rom_set.cpp
    src/rip/rom_set.h
    src/rip/snapshot.cpp
    src/rip/snapshot.h
    src/rip/trim.cpp
    src/rip/trim.h
)
target_link_libraries(usf_rip PUBLIC usf_write xsflib::rip xsflib::rom_set)
target_compile_options(usf_rip PRIVATE ${USF_WARNINGS})

# The player of the front ends: one rate for each track, on xsflib's playback framework.
add_library(usf_player STATIC
    src/usf/uniform_rate_player.cpp
    src/usf/uniform_rate_player.h
)
target_link_libraries(usf_player PUBLIC usf_format xsflib::player)
target_compile_options(usf_player PRIVATE ${USF_WARNINGS})

# n64run boots a cartridge, or loads a USF track, runs it and writes what its audio interface plays to a WAV file. On
# Windows, its code page is UTF-8.
add_executable(n64run src/tools/n64run.cpp)
target_link_libraries(n64run PRIVATE usf_format)
target_compile_options(n64run PRIVATE ${USF_WARNINGS})
xsflib_add_utf8_manifest(n64run)

# usfrip rips a game's music into a USF set, with the game's profile. On Windows, its code page is UTF-8.
add_executable(usfrip src/tools/usfrip.cpp)
target_link_libraries(usfrip PRIVATE usf_rip xsflib::console)
target_compile_options(usfrip PRIVATE ${USF_WARNINGS})
xsflib_add_version(usfrip VERSION ${usf_VERSION} DEFINE USF_VERSION DESCRIPTION "USF ripper" PRODUCT usf)
xsflib_add_utf8_manifest(usfrip)

# usfplay renders USF tracks to WAV or FLAC files. On Windows, its code page is UTF-8. On macOS, a droplet app takes files
# dropped on it in the Finder.
add_executable(usfplay src/tools/usfplay.cpp)
target_link_libraries(usfplay PRIVATE usf_player xsflib::cli)
target_compile_options(usfplay PRIVATE ${USF_WARNINGS})
xsflib_add_version(usfplay VERSION ${usf_VERSION} DEFINE USF_VERSION DESCRIPTION "USF renderer" PRODUCT usf
                   COPYRIGHT "Copyright (c) 2026 the usf authors. MIT licence.")
xsflib_add_utf8_manifest(usfplay)
xsflib_add_droplet(usfplay VERSION ${usf_VERSION})

# foobar2000 component (Windows with clang-cl, or macOS): cmake -DFOOBAR2000_SDK_DIR=<unpacked SDK> ...
set(FOOBAR2000_SDK_DIR "" CACHE PATH "foobar2000 SDK (builds the foo_input_usfplay component when set)")
if (FOOBAR2000_SDK_DIR)
    if (NOT MSVC AND NOT APPLE)
        message(FATAL_ERROR "the foobar2000 component requires clang-cl on Windows, or macOS")
    endif()
    add_subdirectory(plugins/foobar2000)
endif()

# XMPlay plugin (32-bit Windows with clang-cl): cmake -A Win32 -T ClangCL -DXMPLAY_SDK_DIR=<unpacked SDK> ...
set(XMPLAY_SDK_DIR "" CACHE PATH "XMPlay plugin SDK (builds the xmp-usfplay plugin when set)")
if (XMPLAY_SDK_DIR AND WIN32)
    add_subdirectory(plugins/xmplay)
endif()

option(USF_TESTS "Build usf's tests and register them with CTest." ON)
if (USF_TESTS)
    enable_testing()

    # The unit tests: usf_<name>_test from tests/<name>_test.cpp, registered as usf_<name>.
    xsflib_add_project_test(usf save_state usf_n64)
    xsflib_add_project_test(usf cpu usf_n64)
    xsflib_add_project_test(usf legacy_timing usf_n64)
    xsflib_add_project_test(usf fast_path usf_rip)
    xsflib_add_project_test(usf rsp_vector usf_n64)
    xsflib_add_project_test(usf fpu usf_n64)
    xsflib_add_project_test(usf usf_file usf_write)
    xsflib_add_project_test(usf mips usf_rip)
    xsflib_add_project_test(usf rip usf_rip)
    xsflib_add_project_test(usf player usf_rip)
    xsflib_add_project_test(usf pif usf_n64)
    xsflib_add_project_test(usf aleck64 usf_n64)
    xsflib_add_project_test(usf rom_set usf_rip)

    # On Windows, UTF-8 is the code page of the tests that open files by a narrow path, and of the programs that take
    # paths on the command line. A temporary or build folder can then have a name outside ASCII.
    xsflib_add_utf8_manifest(usf_rip_test)
    xsflib_add_utf8_manifest(usf_player_test)

    # A USF set made from small programs, with no game data. The tests of the front ends play it.
    add_library(usf_test_set STATIC tests/test_set.cpp tests/test_set.h)
    target_include_directories(usf_test_set PUBLIC tests)
    target_link_libraries(usf_test_set PUBLIC usf_rip)
    target_compile_options(usf_test_set PRIVATE ${USF_WARNINGS})
    xsflib_add_project_test(usf front_end usf_player usf_test_set)

    add_executable(usf_make_test_set tests/make_test_set.cpp)
    target_link_libraries(usf_make_test_set PRIVATE usf_test_set)
    target_compile_options(usf_make_test_set PRIVATE ${USF_WARNINGS})
    xsflib_add_utf8_manifest(usf_make_test_set)
    set(usf_test_set_folder ${CMAKE_CURRENT_BINARY_DIR}/test-set)
    add_test(NAME usf_test_set_files COMMAND usf_make_test_set ${usf_test_set_folder})
    set_tests_properties(usf_test_set_files PROPERTIES FIXTURES_SETUP usf_test_set)

    # usfplay's WAV files against the core's output, on the set above.
    add_executable(usf_play_sets tests/expect_file.h tests/play_sets.cpp)
    target_link_libraries(usf_play_sets PRIVATE usf_format xsflib::testing seqlib::seqlib)
    target_compile_options(usf_play_sets PRIVATE ${USF_WARNINGS})
    xsflib_add_utf8_manifest(usf_play_sets)
    add_test(NAME usfplay_test_set COMMAND usf_play_sets ${usf_test_set_folder} $<TARGET_FILE:usfplay>
                                           ${CMAKE_CURRENT_BINARY_DIR}/play-test-set)
    set_tests_properties(usfplay_test_set PROPERTIES FIXTURES_REQUIRED usf_test_set)

    # usfplay reports the version it was built as.
    xsflib_add_version_test(usfplay VERSION ${usf_VERSION})

    # The test ROMs aren't in the repo. USF_TEST_ROMS names a folder that holds them: n64-systemtest.z64 from
    # n64-systemtest, and PeterLemon's N64 tests in their repository's layout under PeterLemon/.
    set(USF_TEST_ROMS "" CACHE PATH "Folder of the test ROMs (n64-systemtest and PeterLemon's N64 tests)")
    add_executable(usf_test_rom tests/test_rom.cpp)
    target_link_libraries(usf_test_rom PRIVATE usf_n64)
    target_compile_options(usf_test_rom PRIVATE ${USF_WARNINGS})
    xsflib_add_utf8_manifest(usf_test_rom)
    if (USF_TEST_ROMS)
        include(tests/test_roms.cmake)
    endif()

    # Load local_tests.cmake from each immediate subdirectory to register tests that use locally supplied files.
    xsflib_include_local_tests()
endif()
