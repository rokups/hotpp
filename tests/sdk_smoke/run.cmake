# Copyright (c) 2026 the Hot++ project.
# This work is licensed under the terms of the MIT license.
# For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

# The SDK smoke test. It configures, builds and runs the project in this folder against an
# installed Hot++ SDK. CI runs it on Linux and on Windows. You can run it locally too:
#
#   cmake -DSDK=<sdk folder> -P tests/sdk_smoke/run.cmake
#
# Variables (-D):
#   SDK             Required. The SDK prefix (it has lib/cmake/Hot), or a folder with one such prefix.
#   WORK_DIR        The build folder of the test. Default: sdk_smoke_build in the current folder.
#                   The script removes it first.
#   GENERATOR       The CMake generator. Default: Ninja. The value "default" gives no -G, so
#                   CMake uses its default generator (on Windows: the newest Visual Studio).
#   CONFIG          The build type. Default: Release.
#   CXX_COMPILER    The C++ compiler. Default: the CMake default.
#   TOOLCHAIN_FILE  A toolchain file, for a cross test (for example a Windows SDK under wine).
#   EMULATOR        A program that runs the test executable (for example wine).
#   EXTRA_ARGS      More configure arguments, as a CMake list.
# The test passes when the program prints "sdk_smoke: result=46" and returns 0.
cmake_minimum_required(VERSION 3.20)

if(NOT SDK)
    message(FATAL_ERROR "run.cmake needs -DSDK=<the folder of the SDK>")
endif()
get_filename_component(SDK "${SDK}" ABSOLUTE)
if(NOT EXISTS "${SDK}/lib/cmake/Hot/HotConfig.cmake")
    # An unpacked archive has one top folder. Use the prefix in it.
    file(GLOB _found "${SDK}/*/lib/cmake/Hot/HotConfig.cmake")
    list(LENGTH _found _count)
    if(NOT _count EQUAL 1)
        message(FATAL_ERROR "No Hot++ SDK in ${SDK}. The folder must have lib/cmake/Hot/HotConfig.cmake, "
            "or one subfolder with it.")
    endif()
    get_filename_component(SDK "${_found}/../../../.." ABSOLUTE)
endif()
message(STATUS "SDK: ${SDK}")

if(NOT WORK_DIR)
    set(WORK_DIR "${CMAKE_BINARY_DIR}/sdk_smoke_build")
endif()
get_filename_component(WORK_DIR "${WORK_DIR}" ABSOLUTE)
if(NOT GENERATOR)
    set(GENERATOR Ninja)
endif()
if(NOT CONFIG)
    set(CONFIG Release)
endif()

# Run a command. Stop with an error when it fails.
function(_run)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "Failed (${rc}): ${ARGN}")
    endif()
endfunction()

file(REMOVE_RECURSE "${WORK_DIR}")
set(configure_args -S "${CMAKE_CURRENT_LIST_DIR}" -B "${WORK_DIR}"
    "-DCMAKE_PREFIX_PATH=${SDK}" "-DCMAKE_BUILD_TYPE=${CONFIG}")
if(NOT GENERATOR STREQUAL "default")
    list(APPEND configure_args -G "${GENERATOR}")
endif()
if(CXX_COMPILER)
    list(APPEND configure_args "-DCMAKE_CXX_COMPILER=${CXX_COMPILER}")
endif()
if(TOOLCHAIN_FILE)
    list(APPEND configure_args "-DCMAKE_TOOLCHAIN_FILE=${TOOLCHAIN_FILE}")
endif()
_run(${CMAKE_COMMAND} ${configure_args} ${EXTRA_ARGS})
_run(${CMAKE_COMMAND} --build "${WORK_DIR}" --config ${CONFIG})

# A single-config generator puts the program in WORK_DIR. A multi-config one adds /<CONFIG>.
file(GLOB program "${WORK_DIR}/sdk_smoke" "${WORK_DIR}/sdk_smoke.exe"
    "${WORK_DIR}/${CONFIG}/sdk_smoke" "${WORK_DIR}/${CONFIG}/sdk_smoke.exe")
if(NOT program)
    message(FATAL_ERROR "The build did not make the program sdk_smoke in ${WORK_DIR}")
endif()
list(GET program 0 program)
execute_process(COMMAND ${EMULATOR} "${program}" RESULT_VARIABLE rc
    OUTPUT_VARIABLE out ERROR_VARIABLE err)
message(STATUS "Output of sdk_smoke:\n${out}${err}")
if(NOT rc EQUAL 0 OR NOT out MATCHES "sdk_smoke: result=46")
    message(FATAL_ERROR "The SDK smoke test failed (exit code ${rc}). Expected: sdk_smoke: result=46")
endif()
message(STATUS "The SDK smoke test passed")
