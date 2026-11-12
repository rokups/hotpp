# Copyright (c) 2026 the Hot++ project.
# This work is licensed under the terms of the MIT license.
# For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

# Closures that outlive a generation, and the unload of a retired generation.
add_executable(integration_closure_tests
    closure_tests.cpp
    generation_unload_tests.cpp
    lambda_closure_tests.cpp
    shared_cell_tests.cpp
    ${PROJECT_SOURCE_DIR}/tests/engine/Actor.cpp
    ${PROJECT_SOURCE_DIR}/tests/engine/Util.cpp)
target_include_directories(integration_closure_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests)
target_link_libraries(integration_closure_tests PRIVATE hot_backend GTest::gtest_main)
set_target_properties(integration_closure_tests PROPERTIES ENABLE_EXPORTS ON)
target_compile_definitions(integration_closure_tests PRIVATE HOT_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
hot_gtest(integration_closure_tests)

# A lambda in a method that never runs registers its record at module load. The box lambdas of a
# .hot script run in each mode. The AOT code of the scripts goes into the executable. The tests
# load it with Loader::load_aot.
hot_add_scripts(TARGET lambda_reg_aot_scripts AOT
    SCRIPTS ${PROJECT_SOURCE_DIR}/tests/scripts/lambda_reg.cpp ${PROJECT_SOURCE_DIR}/tests/scripts/lambda_box.hot
    INCLUDES ${PROJECT_SOURCE_DIR}/tests/engine
    DEPENDS ${HOT_HEADERS})
add_executable(integration_lambda_reg_tests
    lambda_box_tests.cpp
    lambda_reg_tests.cpp
    ${PROJECT_SOURCE_DIR}/tests/engine/Actor.cpp
    ${PROJECT_SOURCE_DIR}/tests/engine/Util.cpp)
target_include_directories(integration_lambda_reg_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests)
target_link_libraries(integration_lambda_reg_tests PRIVATE lambda_reg_aot_scripts hot_backend GTest::gtest_main)
set_target_properties(integration_lambda_reg_tests PROPERTIES ENABLE_EXPORTS ON)
target_compile_definitions(integration_lambda_reg_tests PRIVATE HOT_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
hot_gtest(integration_lambda_reg_tests)
