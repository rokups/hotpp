# Copyright (c) 2026 the Hot++ project.
# This work is licensed under the terms of the MIT license.
# For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

# The test of HOT_HOTC_EXECUTABLE. Run it with "cmake -P". It does these steps:
#   1. configure samples/aot with the translator off and HOT_HOTC_EXECUTABLE set to a built hotc
#   2. check that the project has no target hotc
#   3. build one AOT sample with the prebuilt hotc and run it
#   4. check that a cross build with no hotc gives the error message (a Linux host only)
#   5. remove the build folders.
# Variables (-D): HOTC, SAMPLE_DIR, WORK_DIR, CXX_COMPILER, GENERATOR.
foreach(v HOTC SAMPLE_DIR WORK_DIR CXX_COMPILER GENERATOR)
    if(NOT DEFINED ${v})
        message(FATAL_ERROR "HotcExecutableTest.cmake needs -D${v}=...")
    endif()
endforeach()

function(_run)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "Failed (${rc}): ${ARGN}")
    endif()
endfunction()

set(build ${WORK_DIR}/hotc_exe)
file(REMOVE_RECURSE ${build})
_run(${CMAKE_COMMAND} -S ${SAMPLE_DIR} -B ${build} -G ${GENERATOR}
     -DHOT_ROOT=${SAMPLE_DIR}/../.. -DHOT_BUILD_TRANSLATOR=OFF -DHOT_BUILD_TESTS=OFF
     -DHOT_BUILD_VSCODE=OFF -DHOT_HOTC_EXECUTABLE=${HOTC}
     -DCMAKE_CXX_COMPILER=${CXX_COMPILER} -DCMAKE_BUILD_TYPE=Release -DCMAKE_CONFIGURATION_TYPES=Release)

# The project must not have the target hotc.
if(GENERATOR MATCHES "Visual Studio")
    # This generator has no target "help": a target is a project file hotc.vcxproj.
    if(EXISTS ${build}/hotc.vcxproj)
        message(FATAL_ERROR "The project has the target hotc (${build}/hotc.vcxproj)")
    endif()
else()
    execute_process(COMMAND ${CMAKE_COMMAND} --build ${build} --target help
                    OUTPUT_VARIABLE targets RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0 OR targets MATCHES "(^|\n)(\\.\\.\\. )?hotc(:| |\n|$)")
        message(FATAL_ERROR "The project has the target hotc, or the target list failed:\n${targets}")
    endif()
endif()

_run(${CMAKE_COMMAND} --build ${build} --target sample_aot_hot --config Release)
# The folder of the program depends on the generator (a configuration folder, or none).
file(GLOB_RECURSE exe ${build}/sample_aot_hot ${build}/sample_aot_hot.exe)
list(GET exe 0 exe)
_run(${exe})

# A cross build with no hotc stops with a clear message. The fake cross build uses a Linux
# system name, so it runs on a Linux host only.
if(NOT CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
    file(REMOVE_RECURSE ${build})
    return()
endif()
set(cross ${WORK_DIR}/hotc_exe_cross)
file(REMOVE_RECURSE ${cross})
execute_process(
    COMMAND ${CMAKE_COMMAND} -S ${SAMPLE_DIR} -B ${cross} -G ${GENERATOR}
        -DHOT_ROOT=${SAMPLE_DIR}/../.. -DHOT_BUILD_TRANSLATOR=OFF -DHOT_BUILD_TESTS=OFF
        -DHOT_BUILD_VSCODE=OFF -DCMAKE_CXX_COMPILER=${CXX_COMPILER}
        -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_CROSSCOMPILING=ON
    ERROR_VARIABLE err RESULT_VARIABLE rc)
if(rc EQUAL 0 OR NOT err MATCHES "HOT_HOTC_EXECUTABLE")
    message(FATAL_ERROR "The cross build did not stop with the HOT_HOTC_EXECUTABLE message (rc ${rc}):\n${err}")
endif()

file(REMOVE_RECURSE ${build} ${cross})
