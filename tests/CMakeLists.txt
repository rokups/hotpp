# The tests. Each folder has its own CMakeLists.txt.
# The translator tests stay here for now. They keep their targets and their files.

# The cache of the translator session needs only spc_lib.
add_executable(translator_tests translator_tests.cpp)
target_link_libraries(translator_tests PRIVATE spc_lib GTest::gtest_main)
target_include_directories(translator_tests PRIVATE ${CMAKE_SOURCE_DIR}/src/translator/parser ${CMAKE_SOURCE_DIR}/src/translator)
target_compile_definitions(translator_tests PRIVATE SPP_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
gtest_discover_tests(translator_tests)

# The printer of the expression and statement AST.
add_executable(emit_tests emit_tests.cpp)
target_link_libraries(emit_tests PRIVATE spc_lib GTest::gtest_main)
target_include_directories(emit_tests PRIVATE ${CMAKE_SOURCE_DIR}/src/translator)
gtest_discover_tests(emit_tests)

# The type model: stubs to C++ storage types.
add_executable(types_tests types_tests.cpp)
target_link_libraries(types_tests PRIVATE spc_lib GTest::gtest_main)
target_include_directories(types_tests PRIVATE ${CMAKE_SOURCE_DIR}/src/translator)
gtest_discover_tests(types_tests)

# The parser of the expressions: round trip with the printer.
add_executable(expr_parse_tests expr_parse_tests.cpp)
target_link_libraries(expr_parse_tests PRIVATE spc_lib GTest::gtest_main)
target_include_directories(expr_parse_tests PRIVATE ${CMAKE_SOURCE_DIR}/src/translator)
gtest_discover_tests(expr_parse_tests)

# The parser of the statements: round trip with the printer, and the example scripts.
add_executable(stmt_parse_tests stmt_parse_tests.cpp)
target_link_libraries(stmt_parse_tests PRIVATE spc_lib GTest::gtest_main)
target_include_directories(stmt_parse_tests PRIVATE ${CMAKE_SOURCE_DIR}/src/translator)
target_compile_definitions(stmt_parse_tests PRIVATE SPP_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
gtest_discover_tests(stmt_parse_tests)

# The lowering pass.
add_executable(lower_tests lower_tests.cpp)
target_link_libraries(lower_tests PRIVATE spc_lib GTest::gtest_main)
target_include_directories(lower_tests PRIVATE ${CMAKE_SOURCE_DIR}/src/translator)
gtest_discover_tests(lower_tests)

add_subdirectory(unit/runtime)
add_subdirectory(integration)
