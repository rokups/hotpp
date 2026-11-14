# Copyright (c) 2026 the Hot++ project.
# This work is licensed under the terms of the MIT license.
# For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

# The test of the installed SDK. Run it with "cmake -P". It does these steps:
#   1. cmake --install <BUILD_DIR> --prefix <WORK_DIR>/prefix, then move the prefix to
#      <WORK_DIR>/prefix_moved
#   2. configure samples/aot against the prefix (HOT_ROOT is empty, so find_package(Hot) runs)
#   3. build the sample and run its tests.
# Variables (-D): BUILD_DIR, SAMPLE_DIR, WORK_DIR, CXX_COMPILER, CXX_FLAGS, GENERATOR, CONFIG,
# SCRIPT_COMPILER. SCRIPT_COMPILER is the HOT_SCRIPT_COMPILER of the build. Without it, the SDK
# on MSVC takes the clang++ of Visual Studio. That clang++ can have an other LLVM major version.
foreach(v BUILD_DIR SAMPLE_DIR WORK_DIR CXX_COMPILER GENERATOR)
    if(NOT DEFINED ${v})
        message(FATAL_ERROR "SdkInstallTest.cmake needs -D${v}=...")
    endif()
endforeach()

# Run a command. Stop with an error when it fails.
function(_run)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "Failed (${rc}): ${ARGN}")
    endif()
endfunction()

set(prefix ${WORK_DIR}/prefix)
set(moved_prefix ${WORK_DIR}/prefix_moved)
set(sample_build ${WORK_DIR}/sample_aot)
file(REMOVE_RECURSE ${prefix} ${moved_prefix} ${sample_build})

set(script_compiler_args "")
if(SCRIPT_COMPILER)
    set(script_compiler_args "-DHOT_SCRIPT_COMPILER=${SCRIPT_COMPILER}")
endif()

set(config_args "")
if(CONFIG)
    set(config_args --config ${CONFIG})
endif()
_run(${CMAKE_COMMAND} --install ${BUILD_DIR} --prefix ${prefix} ${config_args})
# Move the prefix. The SDK must work from the new place (it is relocatable).
file(RENAME ${prefix} ${moved_prefix})
set(prefix ${moved_prefix})
_run(${CMAKE_COMMAND} -S ${SAMPLE_DIR} -B ${sample_build} -G ${GENERATOR}
     -DHOT_ROOT= -DCMAKE_PREFIX_PATH=${prefix}
     -DCMAKE_CXX_COMPILER=${CXX_COMPILER} "-DCMAKE_CXX_FLAGS=${CXX_FLAGS}"
     -DCMAKE_BUILD_TYPE=${CONFIG} ${script_compiler_args})
# A native build with the SDK must use the target hot::hotc of the package. The prebuilt hotc
# variable must stay empty.
file(STRINGS ${sample_build}/CMakeCache.txt hotc_exe_line REGEX "^HOT_HOTC_EXECUTABLE:")
if(NOT hotc_exe_line MATCHES "=$")
    message(FATAL_ERROR "HOT_HOTC_EXECUTABLE is not empty in the SDK build: ${hotc_exe_line}")
endif()
_run(${CMAKE_COMMAND} --build ${sample_build} ${config_args})
# Ninja only: one build file must have the hotc of the SDK prefix in an AOT command.
file(GLOB_RECURSE sdk_ninja ${sample_build}/*.ninja)
if(sdk_ninja)
    set(uses_sdk_hotc FALSE)
    foreach(f ${sdk_ninja})
        file(READ ${f} ninja_text)
        if(ninja_text MATCHES "${prefix}[^ \n]*/hotc ")
            set(uses_sdk_hotc TRUE)
        endif()
    endforeach()
    if(NOT uses_sdk_hotc)
        message(FATAL_ERROR "No AOT command uses the hotc of the SDK prefix")
    endif()
endif()
set(ctest_config "")
if(CONFIG)
    set(ctest_config -C ${CONFIG})
endif()
_run(${CMAKE_CTEST_COMMAND} --test-dir ${sample_build} ${ctest_config} --output-on-failure)
