# The tests of the type model: stubs to C++ storage types.
add_executable(unit_translator_types_tests
    default_handle_type_tests.cpp
    inherited_handle_type_tests.cpp
    intrusive_handle_type_tests.cpp
    script_class_type_tests.cpp
    type_model_tests.cpp)
target_link_libraries(unit_translator_types_tests PRIVATE sppc_lib GTest::gtest_main)
target_include_directories(unit_translator_types_tests PRIVATE ${CMAKE_SOURCE_DIR}/src/translator ${CMAKE_SOURCE_DIR}/tests)
target_compile_definitions(unit_translator_types_tests PRIVATE SPP_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
spp_gtest(unit_translator_types_tests)
