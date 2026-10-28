# Language server tests (S14b, design M12 to M14). The test client talks to the server over
# pipes in the test process. A test that needs clangd is skipped when clangd is missing.
find_program(SPP_CLANGD clangd)
if(NOT SPP_CLANGD)
    set(SPP_CLANGD "")
endif()
add_executable(integration_ls_tests ls_tests.cpp)
target_include_directories(integration_ls_tests PRIVATE ${CMAKE_SOURCE_DIR}/tests)
target_link_libraries(integration_ls_tests PRIVATE spp_ls_lib GTest::gtest_main)
target_compile_definitions(integration_ls_tests PRIVATE
    SPP_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    SPP_CLANGD_PATH="${SPP_CLANGD}"
    SPP_RESOURCE_DIR="${CLANG_RESOURCE_DIR}")
gtest_discover_tests(integration_ls_tests DISCOVERY_TIMEOUT 30)
