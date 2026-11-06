# SPDX-License-Identifier: MIT

# The tests that run test ROMs from USF_TEST_ROMS. Neither suite is in the repo.
#
# - n64-systemtest (MIT): n64-systemtest.z64, built from its source.
# - PeterLemon's N64 tests (Unlicense): their repository's CPUTest and RSPTest folders under PeterLemon/, with the
#   ROMs and the fonts beside each test's source.

if (EXISTS "${USF_TEST_ROMS}/n64-systemtest.z64")
    add_test(NAME usf_systemtest COMMAND usf_test_rom systemtest "${USF_TEST_ROMS}/n64-systemtest.z64")
endif()

# PeterLemon's tests that don't draw a verdict the usual way:
# - COP1FullMode and FPUCompare draw values but no PASS or FAIL. Their screenshots in the tests' repository are the
#   reference. A screenshot shows the frame's 240 lines doubled, and the hashes below are of every other line of it.
# - The two TIMINGNTSC tests count the instructions that run in one frame. The count depends on the time that each
#   instruction and bus access takes, which the core doesn't model.
# - Six of the RESERVED tests also check a second reserved vector instruction. Their expected accumulator values for
#   it disagree with n64-systemtest's tests of the same instructions, which were checked on hardware. The core follows
#   n64-systemtest.
set(usf_peterlemon_skipped
    CPUTest/CP1/COP1FullMode CPUTest/CP1/C/FPUCompare CPUTest/CPU/TIMINGNTSC CPUTest/CP1/TIMINGNTSC
    RSPTest/CP2/RESERVED/V073 RSPTest/CP2/RESERVED/VACC RSPTest/CP2/RESERVED/VEXTN RSPTest/CP2/RESERVED/VEXTQ
    RSPTest/CP2/RESERVED/VEXTT RSPTest/CP2/RESERVED/VSAC)

set(usf_peterlemon_frames
    "CPUTest/CP1/COP1FullMode/COP1FullMode.N64=8bda5e2ded8b138a0f5f173588822efea436ca10"
    "CPUTest/CP1/C/FPUCompare/FPUCompare-c.eq.s.N64=5db950d1c7d3f80f5f5bf4c2cbb55488cb31d6b5"
    "CPUTest/CP1/C/FPUCompare/FPUCompare-c.f.s.N64=f40e58a3286f15befdce9b78de2bb355b8cd7666"
    "CPUTest/CP1/C/FPUCompare/FPUCompare-c.le.s.N64=403a51a6a9fd0798c7c30760cf4a109b0cc44137"
    "CPUTest/CP1/C/FPUCompare/FPUCompare-c.lt.s.N64=21177c7e76d0457d298bbab4732f4eceb96d2c1d"
    "CPUTest/CP1/C/FPUCompare/FPUCompare-c.nge.s.N64=57aa2e5580ca0cf266116bb3ebd15085d8692105"
    "CPUTest/CP1/C/FPUCompare/FPUCompare-c.ngl.s.N64=5a56eec4ca7fde628d80687ae04939e857582e58"
    "CPUTest/CP1/C/FPUCompare/FPUCompare-c.ngle.s.N64=300453ff19e975d5ef95693f34a9b571fd07a773"
    "CPUTest/CP1/C/FPUCompare/FPUCompare-c.ngt.s.N64=1ce577de497b213ac901ea193b54f58f6bff0a9c"
    "CPUTest/CP1/C/FPUCompare/FPUCompare-c.ole.s.N64=ac01fa66efc675f561d0b414c713a369fbded28d"
    "CPUTest/CP1/C/FPUCompare/FPUCompare-c.olt.s.N64=c3c643d25a5d7310fe65081db4ae44890adbfbca"
    "CPUTest/CP1/C/FPUCompare/FPUCompare-c.seq.s.N64=7c185ed0a73613b5c90639e34fd2be06a6734b0e"
    "CPUTest/CP1/C/FPUCompare/FPUCompare-c.sf.s.N64=bd4e49a8549ef72e36c05c665f274331d69b761f"
    "CPUTest/CP1/C/FPUCompare/FPUCompare-c.ueq.s.N64=67489a8c45a1f7b0174df1bec9e5cc4926e9e04c"
    "CPUTest/CP1/C/FPUCompare/FPUCompare-c.ule.s.N64=eb7c1d3d9a5391f6d235bc6a43fa5f2a316f6129"
    "CPUTest/CP1/C/FPUCompare/FPUCompare-c.ult.s.N64=2d7083f0a21991c922f4dc6e94c4fb0eca856683"
    "CPUTest/CP1/C/FPUCompare/FPUCompare-c.un.s.N64=498f86abc26fe8b3ba5f8c9d34f32507a9243c6d")
foreach (entry IN LISTS usf_peterlemon_frames)
    string(REPLACE "=" ";" parts "${entry}")
    list(GET parts 0 rom)
    list(GET parts 1 sha1)
    if (NOT EXISTS "${USF_TEST_ROMS}/PeterLemon/${rom}")
        continue()
    endif()
    string(REGEX REPLACE "\\.[nN]64$" "" name "${rom}")
    string(REPLACE "/" "_" name "${name}")
    add_test(NAME usf_peterlemon_${name} COMMAND usf_test_rom peterlemon-frame "${USF_TEST_ROMS}/PeterLemon/${rom}"
             ${sha1} 240)
endforeach()

file(GLOB_RECURSE usf_peterlemon_roms LIST_DIRECTORIES false
     "${USF_TEST_ROMS}/PeterLemon/CPUTest/*.N64" "${USF_TEST_ROMS}/PeterLemon/CPUTest/*.n64"
     "${USF_TEST_ROMS}/PeterLemon/RSPTest/*.N64" "${USF_TEST_ROMS}/PeterLemon/RSPTest/*.n64")
foreach (rom IN LISTS usf_peterlemon_roms)
    get_filename_component(folder "${rom}" DIRECTORY)
    if (NOT EXISTS "${folder}/FontGreen8x8.bin")
        continue()
    endif()
    file(RELATIVE_PATH relative_folder "${USF_TEST_ROMS}/PeterLemon" "${folder}")
    if (relative_folder IN_LIST usf_peterlemon_skipped)
        continue()
    endif()
    file(RELATIVE_PATH name "${USF_TEST_ROMS}/PeterLemon" "${rom}")
    string(REGEX REPLACE "\\.[nN]64$" "" name "${name}")
    string(REPLACE "/" "_" name "${name}")
    add_test(NAME usf_peterlemon_${name} COMMAND usf_test_rom peterlemon "${rom}")
endforeach()
