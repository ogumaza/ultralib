# SPDX-License-Identifier: MIT

# A project that uses ultralib, to check the CMake package. ULTRALIB_ROUTE selects how the project gets ultralib:
# "subdirectory" adds the source folder, "fetchcontent" declares it with FetchContent, and "find_package" finds an
# installed copy. In the first two, the project adds xsflib and ntdlib from ultralib's submodule first. A project that
# uses the three libraries does the same. To check a route, configure this folder with -DULTRALIB_ROUTE=<route>, build
# it and run ctest. The check links the emulator core and the sound module. ultralib therefore has to be built with
# ULTRALIB_SOUND. The check links the ROM sets too where xsflib builds them. The first two routes build them with zlib.
cmake_minimum_required(VERSION 3.20)
project(ultralib_package_test LANGUAGES C CXX)

set(ULTRALIB_ROUTE "find_package" CACHE STRING "How to get ultralib: subdirectory, fetchcontent or find_package")
set(ULTRALIB_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH "ultralib's source folder")
set(ULTRALIB_NTDLIB_DIR "${ULTRALIB_SOURCE_DIR}/externals/ntdlib")

if (ULTRALIB_ROUTE STREQUAL "subdirectory")
    set(ULTRALIB_SOUND ON)
    set(XSFLIB_ZLIB ON)
    add_subdirectory(${ULTRALIB_NTDLIB_DIR}/externals/xsflib xsflib EXCLUDE_FROM_ALL)
    add_subdirectory(${ULTRALIB_NTDLIB_DIR} ntdlib EXCLUDE_FROM_ALL)
    add_subdirectory(${ULTRALIB_SOURCE_DIR} ultralib EXCLUDE_FROM_ALL)
elseif (ULTRALIB_ROUTE STREQUAL "fetchcontent")
    set(ULTRALIB_SOUND ON)
    set(XSFLIB_ZLIB ON)
    include(FetchContent)
    FetchContent_Declare(xsflib SOURCE_DIR ${ULTRALIB_NTDLIB_DIR}/externals/xsflib)
    FetchContent_Declare(ntdlib SOURCE_DIR ${ULTRALIB_NTDLIB_DIR})
    FetchContent_Declare(ultralib SOURCE_DIR ${ULTRALIB_SOURCE_DIR})
    FetchContent_MakeAvailable(xsflib ntdlib ultralib)
elseif (ULTRALIB_ROUTE STREQUAL "find_package")
    find_package(ultralib 2.0 REQUIRED)
else()
    message(FATAL_ERROR "ULTRALIB_ROUTE is ${ULTRALIB_ROUTE}: use subdirectory, fetchcontent or find_package")
endif()

add_executable(package_test main.cpp)
target_link_libraries(package_test PRIVATE ultralib::machine ultralib::n64 xsflib::testing)
if (TARGET ultralib::rom_set)
    target_link_libraries(package_test PRIVATE ultralib::rom_set)
    target_compile_definitions(package_test PRIVATE ULTRALIB_PACKAGE_ROM_SET=1)
endif()
set_target_properties(package_test PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)

enable_testing()
add_test(NAME package_test COMMAND package_test)
