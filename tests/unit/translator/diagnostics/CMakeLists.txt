# Copyright (c) 2026 the Hot++ project.
# This work is licensed under the terms of the MIT license.
# For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

# The tests of the diagnostics: the line map, the format and the error text of hotc::translate.
add_executable(unit_translator_diagnostics_tests
    diag_format_tests.cpp
    diag_rewrite_tests.cpp
    analyze_tests.cpp
    syntax_error_diagnostics_tests.cpp
    translate_diagnostics_tests.cpp
    async_diagnostics_tests.cpp
    nullcheck_tests.cpp
    lint_tests.cpp
    operator_diagnostics_tests.cpp
    generics_diagnostics_tests.cpp)
target_link_libraries(unit_translator_diagnostics_tests PRIVATE hotc_lib GTest::gtest_main)
target_include_directories(unit_translator_diagnostics_tests PRIVATE ${PROJECT_SOURCE_DIR}/src/translator ${PROJECT_SOURCE_DIR}/tests)
target_compile_definitions(unit_translator_diagnostics_tests PRIVATE HOT_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
hot_gtest(unit_translator_diagnostics_tests)
