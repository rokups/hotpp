# The tests of the cache of sppc::TranslatorSession.
add_executable(unit_translator_session_tests
    session_cache_tests.cpp)
target_link_libraries(unit_translator_session_tests PRIVATE sppc_lib GTest::gtest_main)
target_include_directories(unit_translator_session_tests PRIVATE ${CMAKE_SOURCE_DIR}/src/translator ${CMAKE_SOURCE_DIR}/tests)
target_compile_definitions(unit_translator_session_tests PRIVATE SPP_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
spp_gtest(unit_translator_session_tests)
