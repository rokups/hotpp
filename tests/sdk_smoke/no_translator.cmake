# Copyright (c) 2026 the Hot++ project.
# This work is licensed under the terms of the MIT license.
# For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

# The SDK smoke case for an SDK without hot_ls (HOT_BUILD_LS=OFF) or without the translator
# (HOT_BUILD_TRANSLATOR=OFF). It checks that find_package(Hot CONFIG) configures, and that the
# targets listed in ABSENT do not exist. Use:
#
#   cmake -DSDK=<sdk prefix> -DABSENT="hot::hot_ls;hot::hotc;hot::hotc_lib" \
#         -DCXX_COMPILER=<c++> -P tests/sdk_smoke/no_translator.cmake
#
# Variables (-D): SDK (required), ABSENT (required), WORK_DIR (default no_translator_build in the
# current folder), CXX_COMPILER (default: the CMake default), GENERATOR (default: Ninja).
cmake_minimum_required(VERSION 3.20)

if(NOT SDK OR NOT ABSENT)
    message(FATAL_ERROR "no_translator.cmake needs -DSDK=<sdk prefix> and -DABSENT=<targets>")
endif()
get_filename_component(SDK "${SDK}" ABSOLUTE)
if(NOT WORK_DIR)
    set(WORK_DIR "${CMAKE_BINARY_DIR}/no_translator_build")
endif()
get_filename_component(WORK_DIR "${WORK_DIR}" ABSOLUTE)
if(NOT GENERATOR)
    set(GENERATOR Ninja)
endif()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}/src")
set(checks "")
foreach(t IN LISTS ABSENT)
    string(APPEND checks "if(TARGET ${t})\n    message(FATAL_ERROR \"${t} exists\")\nendif()\n")
endforeach()
file(WRITE "${WORK_DIR}/src/CMakeLists.txt" "cmake_minimum_required(VERSION 3.20)
project(hot_no_translator CXX)
find_package(Hot CONFIG REQUIRED)
if(NOT TARGET hot::runtime)
    message(FATAL_ERROR \"hot::runtime is missing\")
endif()
${checks}")

set(args -S "${WORK_DIR}/src" -B "${WORK_DIR}/build" -G "${GENERATOR}" "-DCMAKE_PREFIX_PATH=${SDK}")
if(CXX_COMPILER)
    list(APPEND args "-DCMAKE_CXX_COMPILER=${CXX_COMPILER}")
endif()
execute_process(COMMAND ${CMAKE_COMMAND} ${args} RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "find_package(Hot) failed on the SDK without ${ABSENT} (${rc})")
endif()
message(STATUS "The SDK smoke case without ${ABSENT} passed")
