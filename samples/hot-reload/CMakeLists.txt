# The sample hot-reload: a program that replaces the code of a live script object.
# This file works in the S++ build (add_subdirectory) and as a separate project.
cmake_minimum_required(VERSION 3.20)
project(spp_sample_hot_reload CXX)

# The S++ source tree. A separate build uses it. The S++ build already has the targets.
set(SPP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH "The S++ source tree")
if(NOT TARGET sppc)
    add_subdirectory(${SPP_ROOT} spp)
endif()

# spp_backend has make_default_backend and the loader (spp_runtime).
add_executable(sample_hot_reload main.cpp)
target_include_directories(sample_hot_reload PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(sample_hot_reload PRIVATE spp_backend)
# The generation modules resolve host and runtime symbols from the executable.
set_target_properties(sample_hot_reload PROPERTIES ENABLE_EXPORTS ON)
# The program reads wolf_v1.spp and wolf_v2.spp from this folder at run time.
target_compile_definitions(sample_hot_reload PRIVATE SAMPLE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

enable_testing()
add_test(NAME sample_hot_reload COMMAND sample_hot_reload)
# The output must have the lines of both generations.
set_tests_properties(sample_hot_reload PROPERTIES
    PASS_REGULAR_EXPRESSION "\\[v1\\][^\n]*\n[^\n]*\\[v1\\][^\n]*\n[^\n]*\\[v2\\][^\n]*\n[^\n]*\\[v2\\]")
