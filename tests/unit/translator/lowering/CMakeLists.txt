# The tests of the lowering pass.
add_executable(unit_translator_lowering_tests
    handle_lowering_tests.cpp
    lambda_lowering_tests.cpp
    lowering_error_tests.cpp
    member_access_lowering_tests.cpp
    name_resolution_lowering_tests.cpp
    property_lowering_tests.cpp
    ref_out_lowering_tests.cpp
    statement_lowering_tests.cpp
    string_interpolation_lowering_tests.cpp)
target_link_libraries(unit_translator_lowering_tests PRIVATE sppc_lib GTest::gtest_main)
target_include_directories(unit_translator_lowering_tests PRIVATE ${PROJECT_SOURCE_DIR}/src/translator ${PROJECT_SOURCE_DIR}/tests)
target_compile_definitions(unit_translator_lowering_tests PRIVATE SPP_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
spp_gtest(unit_translator_lowering_tests)
