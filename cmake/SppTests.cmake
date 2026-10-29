# Test helpers.
#
#   spp_gtest(<target> [gtest_discover_tests arguments...])
#
# It registers the GoogleTest cases of the target. Each test requires the fixture spp_scratch.
# The test spp_scratch_clean is the setup of this fixture. It removes the old scratch files
# before the tests start. Use PROPERTIES to give more test properties. Callers must not set
# FIXTURES_REQUIRED in PROPERTIES. The wrapper sets it, and a second value would replace it.
include(GoogleTest)

function(spp_gtest target)
    cmake_parse_arguments(PARSE_ARGV 1 A "" "" "PROPERTIES")
    gtest_discover_tests(${target}
        ${A_UNPARSED_ARGUMENTS}
        PROPERTIES FIXTURES_REQUIRED spp_scratch ${A_PROPERTIES})
endfunction()

add_test(NAME spp_scratch_clean
    COMMAND ${CMAKE_COMMAND} -DKEEP=$<BOOL:${SPP_KEEP_TEMP}> -DDIR=${SPP_SCRATCH_DIR}
            -P ${CMAKE_SOURCE_DIR}/cmake/CleanScratch.cmake)
set_tests_properties(spp_scratch_clean PROPERTIES FIXTURES_SETUP spp_scratch)
