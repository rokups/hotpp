# Copyright (c) 2026 the Hot++ project.
# This work is licensed under the terms of the MIT license.
# For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

# Launcher tests: load an entry script with imports and run its main (include/hot/launcher.h).
if(NOT TARGET hot_launcher)
    return()
endif()
add_executable(integration_launcher_tests launcher_tests.cpp)
target_link_libraries(integration_launcher_tests PRIVATE hot_launcher GTest::gtest_main)
set_target_properties(integration_launcher_tests PROPERTIES ENABLE_EXPORTS ON)
target_include_directories(integration_launcher_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests)
target_compile_definitions(integration_launcher_tests PRIVATE
    HOT_SCRATCH_DIR="${HOT_SCRATCH_DIR}/tests/launcher"
    HOT_LAUNCHER_TEST_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
hot_gtest(integration_launcher_tests)
