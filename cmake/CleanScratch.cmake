# Remove the scratch files of the tests, the backends and the benchmarks.
# Run it as a script: cmake -DKEEP=<bool> -DDIR=<scratch dir> -P CleanScratch.cmake
# The directory imgui stays: it is the work directory of the sample `samples/imgui`.
set(_keep "${KEEP}")
if(DEFINED ENV{SPP_KEEP_TEMP})
    if(NOT "$ENV{SPP_KEEP_TEMP}" STREQUAL "" AND NOT "$ENV{SPP_KEEP_TEMP}" STREQUAL "0")
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
file(REMOVE_RECURSE ${DIR}/tests ${DIR}/spp_work ${DIR}/bench)
