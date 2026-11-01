# Copyright (c) 2026 the Hot++ project.
# This work is licensed under the terms of the MIT license.
# For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

# The tests of the printer of the expression and statement tree.
add_executable(unit_translator_emit_tests
    expression_emit_tests.cpp
    lambda_emit_tests.cpp
    line_directive_emit_tests.cpp
    statement_emit_tests.cpp)
target_link_libraries(unit_translator_emit_tests PRIVATE hotc_lib GTest::gtest_main)
target_include_directories(unit_translator_emit_tests PRIVATE ${PROJECT_SOURCE_DIR}/src/translator ${PROJECT_SOURCE_DIR}/tests)
target_compile_definitions(unit_translator_emit_tests PRIVATE HOT_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
hot_gtest(unit_translator_emit_tests)
