# Copyright (c) 2026 the Hot++ project.
# This work is licensed under the terms of the MIT license.
# For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

# Remove the scratch files of the tests, the backends and the benchmarks.
# Run it as a script: cmake -DKEEP=<bool> -DDIR=<scratch dir> -P CleanScratch.cmake
# The directory imgui stays: it is the work directory of the sample `samples/imgui`.
set(_keep "${KEEP}")
if(DEFINED ENV{HOT_KEEP_TEMP})
    if(NOT "$ENV{HOT_KEEP_TEMP}" STREQUAL "" AND NOT "$ENV{HOT_KEEP_TEMP}" STREQUAL "0")
        set(_keep ON)
    else()
        set(_keep OFF)
    endif()
endif()
if(_keep)
    message(STATUS "keep: the scratch files stay in ${DIR}")
    return()
endif()
if("${DIR}" STREQUAL "")
    message(FATAL_ERROR "CleanScratch.cmake needs -DDIR=<scratch dir>")
endif()
file(REMOVE_RECURSE ${DIR}/tests ${DIR}/hot_work ${DIR}/bench)
