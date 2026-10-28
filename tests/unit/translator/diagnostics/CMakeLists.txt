# The tests of the diagnostics: the line map, the format and the error text of sppc::translate.
add_executable(unit_translator_diagnostics_tests
    diag_format_tests.cpp
    diag_rewrite_tests.cpp
    analyze_tests.cpp
    syntax_error_diagnostics_tests.cpp
    translate_diagnostics_tests.cpp)
target_link_libraries(unit_translator_diagnostics_tests PRIVATE sppc_lib GTest::gtest_main)
target_include_directories(unit_translator_diagnostics_tests PRIVATE ${CMAKE_SOURCE_DIR}/src/translator ${CMAKE_SOURCE_DIR}/tests)
target_compile_definitions(unit_translator_diagnostics_tests PRIVATE SPP_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
gtest_discover_tests(unit_translator_diagnostics_tests)
