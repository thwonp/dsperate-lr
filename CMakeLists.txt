# SPDX-License-Identifier: GPL-3.0-or-later
# dsperate_libretro: the libretro core. DSperate's core library is built by
# DSperate's own CMake (build.sh), not as a subdirectory of this project: its
# committed PGO profile names each object by its path in DSperate's build
# tree, so that tree has to be DSperate's own for the profile to apply.
cmake_minimum_required(VERSION 3.16)
project(dsperate_libretro LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(DSPERATE_BUILD "" CACHE PATH "DSperate build directory holding src/core/libdsperate_core.a")
if(NOT EXISTS "${DSPERATE_BUILD}/src/core/libdsperate_core.a")
  message(FATAL_ERROR "DSPERATE_BUILD=${DSPERATE_BUILD} has no src/core/libdsperate_core.a (run build.sh)")
endif()

execute_process(COMMAND git -C "${CMAKE_SOURCE_DIR}/DSperate" describe --tags --always
                OUTPUT_VARIABLE DSPERATE_DESCRIBE OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)

add_library(dsperate_libretro SHARED src/libretro.cpp)
set_target_properties(dsperate_libretro PROPERTIES PREFIX "" INTERPROCEDURAL_OPTIMIZATION ON)
target_include_directories(dsperate_libretro PRIVATE include DSperate/src)
# The core's PUBLIC definitions, as DSperate's build gave them to it.
target_compile_definitions(dsperate_libretro PRIVATE DSPERATE_JIT=1 DSPERATE_NEON=1
                           DSPERATE_LR_VERSION="${DSPERATE_DESCRIBE}")
target_link_libraries(dsperate_libretro PRIVATE "${DSPERATE_BUILD}/src/core/libdsperate_core.a" pthread dl)
# Only the libretro API leaves the library.
target_link_options(dsperate_libretro PRIVATE -Wl,--version-script=${CMAKE_SOURCE_DIR}/src/link.T -Wl,--no-undefined)
