# Copyright (c) 2026 the Hot++ project.
# This work is licensed under the terms of the MIT license.
# For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

# The test no_export_all_symbols: no CMake file of the source tree exports all symbols of a
# target. Only the macro HOT_EXPORT exports a symbol (include/hot/runtime/export.h).
#   cmake -DROOT=<the source tree> -P check_no_export_all.cmake
# The script makes the option names from parts, so it does not find itself.
cmake_minimum_required(VERSION 3.20)
if(NOT ROOT)
    message(FATAL_ERROR "check_no_export_all.cmake needs -DROOT=<the source tree>")
endif()
set(forbidden "WINDOWS_EXPORT_ALL_" "SYMBOLS" "--export-all-" "symbols")
list(SUBLIST forbidden 0 2 msvc_parts)
list(SUBLIST forbidden 2 2 gnu_parts)
string(CONCAT msvc_option ${msvc_parts})
string(CONCAT gnu_option ${gnu_parts})
# The build folders can be in the source tree. The script reads only the folders of the source.
set(files "${ROOT}/CMakeLists.txt" "${ROOT}/CMakePresets.json")
foreach(dir cmake src tests samples tools)
    file(GLOB_RECURSE found "${ROOT}/${dir}/CMakeLists.txt" "${ROOT}/${dir}/*.cmake" "${ROOT}/${dir}/*.cmake.in")
    list(APPEND files ${found})
endforeach()
set(bad "")
foreach(f IN LISTS files)
    file(READ "${f}" text)
    foreach(option "${msvc_option}" "${gnu_option}")
        string(FIND "${text}" "${option}" at)
        if(NOT at EQUAL -1)
            list(APPEND bad "${f}: ${option}")
        endif()
    endforeach()
endforeach()
list(LENGTH files count)
if(bad)
    list(JOIN bad "\n  " text)
    message(FATAL_ERROR "Only HOT_EXPORT exports a symbol. Remove these options:\n  ${text}")
endif()
message(STATUS "No export of all symbols in ${count} CMake files")
