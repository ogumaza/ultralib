# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.20)
project(ultralib VERSION 1.0.0 LANGUAGES C CXX)

# CMake 3.20 has no PROJECT_IS_TOP_LEVEL. ultralib is the top-level project when the build starts in its folder.
if (CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    set(ULTRALIB_TOP_LEVEL ON)
else()
    set(ULTRALIB_TOP_LEVEL OFF)
endif()

option(ULTRALIB_SOUND "Build the readers, writers and models of the N64's sound data. They require ntdlib and seqlib."
       ${ULTRALIB_TOP_LEVEL})
option(ULTRALIB_SEQUENCE_EXPORT "Build the conversions to MIDI, SoundFont 2 and SFZ. They require ULTRALIB_SOUND."
       ${ULTRALIB_TOP_LEVEL})
option(ULTRALIB_TESTS "Build ultralib's tests and register them with CTest." ${ULTRALIB_TOP_LEVEL})
option(ULTRALIB_INSTALL "Install ultralib's headers, libraries and CMake package. Requires installed dependencies." OFF)

if (ULTRALIB_SEQUENCE_EXPORT AND NOT ULTRALIB_SOUND)
    message(FATAL_ERROR "ULTRALIB_SEQUENCE_EXPORT requires ULTRALIB_SOUND.")
endif()

if (ULTRALIB_TOP_LEVEL AND NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# xsflib, and ntdlib for the sound modules. A project that uses ultralib adds them first. A build of ultralib alone adds
# the copies in externals/ntdlib, or finds installed ones. With ULTRALIB_INSTALL, the copies are installed too. The
# emulator core requires xsflib alone.
if (NOT TARGET xsflib::common)
    if (EXISTS ${PROJECT_SOURCE_DIR}/externals/ntdlib/externals/xsflib/CMakeLists.txt)
        set(XSFLIB_PSF ${ULTRALIB_TOP_LEVEL})
        set(XSFLIB_INSTALL ${ULTRALIB_INSTALL})
        add_subdirectory(externals/ntdlib/externals/xsflib)
    else()
        find_package(xsflib 1.10 REQUIRED)
    endif()
endif()
if (ULTRALIB_SOUND AND NOT TARGET ntdlib::common)
    if (EXISTS ${PROJECT_SOURCE_DIR}/externals/ntdlib/CMakeLists.txt)
        set(NTDLIB_SEQUENCE_EXPORT ${ULTRALIB_SEQUENCE_EXPORT})
        set(NTDLIB_INSTALL ${ULTRALIB_INSTALL})
        add_subdirectory(externals/ntdlib)
    else()
        find_package(ntdlib 4.0 REQUIRED)
    endif()
endif()

# seqlib, for the MIDI writers of the sound modules and for the conversions. A build of ultralib alone takes the copy
# that xsflib adds from its submodule, or finds an installed seqlib.
if (ULTRALIB_SOUND AND NOT TARGET seqlib::seqlib)
    find_package(seqlib 1.4 REQUIRED)
endif()

if (ULTRALIB_TESTS)
    enable_testing()
endif()

# The program builders and the test ROM's helpers for the core's tests, beside xsflib's checks.
add_library(ultralib_testing INTERFACE)
add_library(ultralib::testing ALIAS ultralib_testing)
target_include_directories(ultralib_testing INTERFACE $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
                                                    $<INSTALL_INTERFACE:include>)
target_link_libraries(ultralib_testing INTERFACE xsflib::testing)
target_compile_features(ultralib_testing INTERFACE cxx_std_20)
set_target_properties(ultralib_testing PROPERTIES EXPORT_NAME testing)
set_property(GLOBAL APPEND PROPERTY ULTRALIB_INSTALLED_TARGETS ultralib_testing)

# The emulator core: the VR4300, the RSP, the RCP's interfaces, the PIF, boot, save states and coverage.
xsflib_add_project_library(ultralib machine src/machine/aleck64.cpp src/machine/boot.cpp src/machine/coverage.cpp
                           src/machine/cpu.cpp src/machine/cpu_cop0.cpp src/machine/cpu_fast.cpp
                           src/machine/cpu_fpu.cpp src/machine/pif.cpp src/machine/rcp.cpp src/machine/rdram.cpp
                           src/machine/rsp.cpp src/machine/rsp_vector.cpp src/machine/save_state.cpp
                           src/machine/system.cpp src/machine/word_set.cpp)
target_link_libraries(ultralib_machine PUBLIC xsflib::common)

# The FPU's arithmetic runs on the host's floating point in the rounding mode that the emulated FCR31 selects, and
# reads the host's exception flags. The compiler must keep each operation between the calls that set the mode and
# read the flags.
if (MSVC)
    set_source_files_properties(src/machine/cpu_fpu.cpp PROPERTIES COMPILE_OPTIONS "/clang:-ffp-model=strict")
else()
    set_source_files_properties(src/machine/cpu_fpu.cpp PROPERTIES COMPILE_OPTIONS "-ffp-model=strict")
endif()

xsflib_add_project_test(ultralib save_state ultralib::machine)
xsflib_add_project_test(ultralib cpu ultralib::machine)
xsflib_add_project_test(ultralib legacy_timing ultralib::machine)
xsflib_add_project_test(ultralib rsp_vector ultralib::machine)
xsflib_add_project_test(ultralib fpu ultralib::machine)
xsflib_add_project_test(ultralib pif ultralib::machine)
xsflib_add_project_test(ultralib aleck64 ultralib::machine)

# The ROM sets of Seta's Aleck64 board: each game's chips, and the reading of a set into a cartridge's image. They read
# the sets through xsflib::rom_set, which xsflib builds only with zlib.
if (TARGET xsflib::rom_set)
    xsflib_add_project_library(ultralib rom_set src/machine/rom_set.cpp)
    target_link_libraries(ultralib_rom_set PUBLIC xsflib::rom_set)
    xsflib_add_project_test(ultralib rom_set ultralib::rom_set)
endif()

if (ULTRALIB_TESTS)
    # The test ROMs aren't in the repo. ULTRALIB_TEST_ROMS names a folder that holds them: n64-systemtest.z64 from
    # n64-systemtest, and PeterLemon's N64 tests in their repository's layout under PeterLemon/.
    set(ULTRALIB_TEST_ROMS "" CACHE PATH "Folder of the test ROMs (n64-systemtest and PeterLemon's N64 tests)")
    add_executable(ultralib_test_rom tests/test_rom.cpp)
    target_link_libraries(ultralib_test_rom PRIVATE ultralib::machine)
    xsflib_set_flags(ultralib_test_rom)
    xsflib_add_utf8_manifest(ultralib_test_rom)
    if (ULTRALIB_TEST_ROMS)
        include(tests/test_roms.cmake)
    endif()
endif()

if (ULTRALIB_SOUND)
    # Some games run their sound threads with the FPU rounding toward zero (round_toward_zero.h). The sources of those
    # models follow the rounding mode at run time.
    function(ultralib_follow_rounding_mode)
        if (MSVC)
            set_source_files_properties(${ARGN} PROPERTIES COMPILE_OPTIONS /fp:strict)
        else()
            set_source_files_properties(${ARGN} PROPERTIES COMPILE_OPTIONS -frounding-math)
        endif()
    endfunction()

    # The readers and writers of libultra's sequence banks and bank files, of standard and compressed MIDI, of VADPCM
    # samples and of the banks of Nintendo EAD's sound engines, and the reader of those engines' sequences. The library
    # also contains the models of the games' sound engines and of the audio microcodes, and a player of those models for
    # a host.
    xsflib_add_project_library(ultralib n64 src/n64/al_player.cpp src/n64/al_player_sequences.cpp src/n64/al_synth.cpp
                               src/n64/al_synth_compact.cpp src/n64/al_synth_standard.cpp
                               src/n64/ambrella_player.cpp src/n64/audio_rsp.cpp src/n64/audio_rsp_compact.cpp
                               src/n64/audio_rsp_ead.cpp src/n64/bank_file.cpp src/n64/ead_bank.cpp
                               src/n64/ead_early.cpp src/n64/ead_early_scripts.cpp src/n64/ead_early_synthesis.cpp
                               src/n64/ead_early_voices.cpp src/n64/ead_later.cpp src/n64/ead_later_layers.cpp
                               src/n64/ead_later_scripts.cpp src/n64/ead_later_synthesis.cpp
                               src/n64/ead_later_tables.cpp src/n64/ead_later_voices.cpp src/n64/ead_middle.cpp
                               src/n64/ead_middle_scripts.cpp src/n64/ead_middle_synthesis.cpp
                               src/n64/ead_middle_voices.cpp src/n64/ead_script.cpp src/n64/ead_tables.cpp
                               src/n64/enhanced_mix.cpp src/n64/enhanced_mix_ops.cpp src/n64/frame_player.cpp
                               src/n64/midi.cpp src/n64/mus_late_player.cpp src/n64/mus_player.cpp
                               src/n64/rom_data.cpp src/n64/sbn.cpp src/n64/sbn_player.cpp
                               src/n64/sbn_player_synthesis.cpp src/n64/seq_file.cpp src/n64/vadpcm.cpp)
    target_link_libraries(ultralib_n64 PUBLIC xsflib::common PRIVATE ntdlib::common ntdlib::wave seqlib::seqlib)
    set(ULTRALIB_N64_ROUNDING_SOURCES src/n64/al_synth.cpp src/n64/al_synth_compact.cpp src/n64/al_synth_standard.cpp
                                    src/n64/ead_later.cpp src/n64/ead_later_layers.cpp src/n64/ead_later_scripts.cpp
                                    src/n64/ead_later_synthesis.cpp src/n64/ead_later_voices.cpp
                                    src/n64/mus_late_player.cpp src/n64/mus_player.cpp)
    ultralib_follow_rounding_mode(${ULTRALIB_N64_ROUNDING_SOURCES})
    xsflib_add_project_test(ultralib n64_formats ultralib::n64)
    xsflib_add_project_test(ultralib n64_ead ultralib::n64)
    xsflib_add_project_test(ultralib n64_al_player ultralib::n64)
    xsflib_add_project_test(ultralib n64_ambrella ultralib::n64)
    xsflib_add_project_test(ultralib n64_enhanced ultralib::n64)
    xsflib_add_project_test(ultralib n64_mus ultralib::n64)
    xsflib_add_project_test(ultralib n64_audio ultralib::n64)
    xsflib_add_project_test(ultralib n64_frame_player ultralib::n64)
    xsflib_add_project_test(ultralib n64_rom_data ultralib::n64)
    xsflib_add_project_test(ultralib n64_sbn ultralib::n64)
endif()

# The conversion of the N64's sound data to MIDI, SoundFont 2 and SFZ files, from a cartridge image or an Aleck64
# game's ROM set, on ntdlib's conversion layer. It reads the ROM sets of arcade boards through xsflib::rom_set. xsflib
# builds that only with zlib.
if (ULTRALIB_SEQUENCE_EXPORT AND TARGET xsflib::rom_set)
    xsflib_add_project_library(ultralib sequence_export_n64
        src/sequence_export/n64/ead.cpp
        src/sequence_export/n64/envelope.cpp
        src/sequence_export/n64/games.cpp
        src/sequence_export/n64/image.cpp
        src/sequence_export/n64/input.cpp
        src/sequence_export/n64/instruments.cpp
        src/sequence_export/n64/libmus.cpp
        src/sequence_export/n64/libultra.cpp
        src/sequence_export/n64/resampler_low_pass.cpp
        src/sequence_export/n64/sbn.cpp)
    target_link_libraries(ultralib_sequence_export_n64 PUBLIC ntdlib::sequence_export ultralib::n64 ultralib::rom_set)
    xsflib_add_project_test(ultralib sequence_export_n64 ultralib::sequence_export_n64)
endif()

if (ULTRALIB_TESTS)
    # Load local_tests.cmake from each immediate subdirectory to register tests that use locally supplied files.
    xsflib_include_local_tests()
endif()

if (ULTRALIB_INSTALL)
    xsflib_install_package(ultralib)
endif()
