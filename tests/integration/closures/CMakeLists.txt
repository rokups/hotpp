# Copyright (c) 2026 the Hot++ project.
# This work is licensed under the terms of the MIT license.
# For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

# Closures that outlive a generation, and the unload of a retired generation.
add_executable(integration_closure_tests
    closure_tests.cpp
    generation_unload_tests.cpp
    ${PROJECT_SOURCE_DIR}/tests/engine/Actor.cpp
    ${PROJECT_SOURCE_DIR}/tests/engine/Util.cpp)
target_include_directories(integration_closure_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests)
target_link_libraries(integration_closure_tests PRIVATE hot_backend GTest::gtest_main)
set_target_properties(integration_closure_tests PROPERTIES ENABLE_EXPORTS ON)
target_compile_definitions(integration_closure_tests PRIVATE HOT_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
hot_gtest(integration_closure_tests)
