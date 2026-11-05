# Copyright (c) 2026 the Hot++ project.
# This work is licensed under the terms of the MIT license.
# For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

# The sample launcher: a runner that loads a .hot file and calls its main.
# A real project builds its own runner. It links its engine and hot::launcher.
# This file works in the Hot++ build (add_subdirectory) and as a separate project.
cmake_minimum_required(VERSION 3.20)
project(hot_sample_launcher CXX)

# The Hot++ source tree. A separate build uses it. The Hot++ build already has the targets.
# An empty HOT_ROOT means: use the installed SDK (find_package(Hot), CMAKE_PREFIX_PATH).
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../CMakeLists.txt")
    set(HOT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH "The Hot++ source tree. Empty: use the installed SDK")
else()
    set(HOT_ROOT "" CACHE PATH "The Hot++ source tree. Empty: use the installed SDK")
endif()
if(NOT TARGET hot::hotc)
    if(HOT_ROOT)
        add_subdirectory(${HOT_ROOT} hot)
    else()
        find_package(Hot REQUIRED)
    endif()
endif()

if(NOT TARGET hot::launcher)
    message(STATUS "sample_runner: skipped (this Hot++ has no target hot::launcher)")
    return()
endif()

add_executable(sample_runner main.cpp)
target_link_libraries(sample_runner PRIVATE hot::launcher)
# The script modules resolve the symbols of the runtime and of the host from the executable.
set_target_properties(sample_runner PROPERTIES ENABLE_EXPORTS ON)
hot_export_host(sample_runner)

# hello.hot returns the number of the arguments: the program name and two more give 3.
add_test(NAME sample_runner_hello COMMAND sample_runner ${CMAKE_CURRENT_SOURCE_DIR}/hello.hot a b)
set_tests_properties(sample_runner_hello PROPERTIES PASS_REGULAR_EXPRESSION "hello from hot: 3 arguments")
add_test(NAME sample_runner_import COMMAND sample_runner ${CMAKE_CURRENT_SOURCE_DIR}/hello_import.hot)
set_tests_properties(sample_runner_import PROPERTIES PASS_REGULAR_EXPRESSION "square of 7 is 49")

enable_testing()
