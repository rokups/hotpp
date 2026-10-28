# The tests of the printer of the expression and statement tree.
add_executable(unit_translator_emit_tests
    expression_emit_tests.cpp
    lambda_emit_tests.cpp
    line_directive_emit_tests.cpp
    ref_out_emit_tests.cpp
    statement_emit_tests.cpp)
target_link_libraries(unit_translator_emit_tests PRIVATE sppc_lib GTest::gtest_main)
target_include_directories(unit_translator_emit_tests PRIVATE ${CMAKE_SOURCE_DIR}/src/translator ${CMAKE_SOURCE_DIR}/tests)
target_compile_definitions(unit_translator_emit_tests PRIVATE SPP_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
gtest_discover_tests(unit_translator_emit_tests)
