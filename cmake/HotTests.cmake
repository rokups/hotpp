# Copyright (c) 2026 the Hot++ project.
# This work is licensed under the terms of the MIT license.
# For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

# Test helpers.
#
#   hot_gtest(<target> [gtest_discover_tests arguments...])
#
# It registers the GoogleTest cases of the target. Each test requires the fixture hot_scratch.
# The test hot_scratch_clean is the setup of this fixture. It removes the old scratch files
# before the tests start. Use PROPERTIES to give more test properties. Callers must not set
# FIXTURES_REQUIRED in PROPERTIES. The wrapper sets it, and a second value would replace it.
include(GoogleTest)

# The dir of this file. CMAKE_CURRENT_LIST_DIR is not valid inside add_test arguments.
set(HOT_CMAKE_DIR ${CMAKE_CURRENT_LIST_DIR})

function(hot_gtest target)
    cmake_parse_arguments(PARSE_ARGV 1 A "" "DISCOVERY_TIMEOUT" "PROPERTIES")
    # The tests use the standard of the project (CMAKE_CXX_STANDARD, default 17).
    # An emulator (wine) starts slowly. The default discovery timeout (5 s) is too short for it,
    # so the timeout is at least 60 s there. A larger value of the caller wins.
    set(_hot_timeout "${A_DISCOVERY_TIMEOUT}")
    if(CMAKE_CROSSCOMPILING_EMULATOR AND (NOT _hot_timeout OR _hot_timeout LESS 60))
        set(_hot_timeout 60)
    endif()
    set(_hot_discovery "")
    if(_hot_timeout)
        set(_hot_discovery DISCOVERY_TIMEOUT ${_hot_timeout})
    endif()
    # A test that loads script modules exports its symbols. On Windows the backend finds the
    # import library of the test next to the executable (hot_export_host).
    get_target_property(_hot_exports ${target} ENABLE_EXPORTS)
    if(_hot_exports)
        hot_export_host(${target})
    endif()
    gtest_discover_tests(${target}
        ${_hot_discovery}
        ${A_UNPARSED_ARGUMENTS}
        PROPERTIES FIXTURES_REQUIRED hot_scratch ${A_PROPERTIES})
endfunction()

add_test(NAME hot_scratch_clean
    COMMAND ${CMAKE_COMMAND} -DKEEP=$<BOOL:${HOT_KEEP_TEMP}> -DDIR=${HOT_SCRATCH_DIR}
            -P ${HOT_CMAKE_DIR}/CleanScratch.cmake)
set_tests_properties(hot_scratch_clean PROPERTIES FIXTURES_SETUP hot_scratch)
