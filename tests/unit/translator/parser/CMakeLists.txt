# The tests of the parsers: expressions, statements, classes, stubs and builtin types.
add_executable(unit_translator_parser_tests
    builtin_type_parse_tests.cpp
    class_member_parse_tests.cpp
    constructor_parse_tests.cpp
    convenience_parse_tests.cpp
    example_script_parse_tests.cpp
    expression_error_parse_tests.cpp
    expression_limit_parse_tests.cpp
    expression_parse_tests.cpp
    lambda_expression_parse_tests.cpp
    local_declaration_parse_tests.cpp
    method_parameter_parse_tests.cpp
    modifier_parse_tests.cpp
    plain_identifier_parse_tests.cpp
    module_decl_parse_tests.cpp
    native_stub_parse_tests.cpp
    property_parse_tests.cpp
    ref_out_expression_parse_tests.cpp
    ref_out_statement_parse_tests.cpp
    statement_error_parse_tests.cpp
    statement_limit_parse_tests.cpp
    statement_parse_tests.cpp
    structured_binding_parse_tests.cpp
    string_interpolation_parse_tests.cpp
    tolerant_parse_tests.cpp
    try_catch_finally_parse_tests.cpp)
target_link_libraries(unit_translator_parser_tests PRIVATE sppc_lib GTest::gtest_main)
target_include_directories(unit_translator_parser_tests PRIVATE ${PROJECT_SOURCE_DIR}/src/translator ${PROJECT_SOURCE_DIR}/tests)
target_compile_definitions(unit_translator_parser_tests PRIVATE HOT_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
hot_gtest(unit_translator_parser_tests)

# The tests of the ".cpp" frontend (libclang). The engine header Actor.h gives the base class.
add_executable(unit_translator_cpp_frontend_tests cpp_frontend_tests.cpp)
target_link_libraries(unit_translator_cpp_frontend_tests PRIVATE sppc_lib GTest::gtest_main)
target_include_directories(unit_translator_cpp_frontend_tests PRIVATE ${PROJECT_SOURCE_DIR}/src/translator)
target_compile_definitions(unit_translator_cpp_frontend_tests PRIVATE HOT_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    HOT_RESOURCE_DIR="${CLANG_RESOURCE_DIR}")
hot_gtest(unit_translator_cpp_frontend_tests TEST_PREFIX cpp_frontend.)
