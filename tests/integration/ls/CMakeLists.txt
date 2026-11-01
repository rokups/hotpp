# Copyright (c) 2026 the Hot++ project.
# This work is licensed under the terms of the MIT license.
# For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

# Language server tests (S14b, design M12 to M14). The test client talks to the server over
# pipes in the test process. A test that needs clangd is skipped when clangd is missing.
# The clangd of the script compiler comes first: it knows the same C++ (on Windows, the clangd
# of the LLVM release, not an older clangd of Visual Studio).
get_filename_component(_hot_script_bin "${HOT_SCRIPT_COMPILER}" DIRECTORY)
find_program(HOT_CLANGD clangd HINTS "${_hot_script_bin}")
if(NOT HOT_CLANGD)
    set(HOT_CLANGD "")
endif()
add_executable(integration_ls_tests ls_tests.cpp)
target_include_directories(integration_ls_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests)
target_link_libraries(integration_ls_tests PRIVATE hot_ls_lib GTest::gtest_main)
target_compile_definitions(integration_ls_tests PRIVATE
    HOT_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    HOT_CLANGD_PATH="${HOT_CLANGD}"
    HOT_RESOURCE_DIR="${CLANG_RESOURCE_DIR}")
hot_gtest(integration_ls_tests DISCOVERY_TIMEOUT 30)

# Unit test of the host files (step 2a). It needs no clangd.
add_executable(integration_ls_hosts_tests hosts_tests.cpp)
target_link_libraries(integration_ls_hosts_tests PRIVATE hot_ls_lib GTest::gtest_main)
hot_gtest(integration_ls_hosts_tests DISCOVERY_TIMEOUT 30)
