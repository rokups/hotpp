# The sample runtime-compile: a program that compiles a script at run time and loads it.
# This file works in the S++ build (add_subdirectory) and as a separate project.
cmake_minimum_required(VERSION 3.20)
project(spp_sample_runtime_compile CXX)

# The S++ source tree. A separate build uses it. The S++ build already has the targets.
set(SPP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH "The S++ source tree")
if(NOT TARGET sppc)
    add_subdirectory(${SPP_ROOT} spp)
endif()

# spp_backend has make_default_backend and the loader (spp_runtime).
add_executable(sample_runtime_compile main.cpp)
target_include_directories(sample_runtime_compile PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(sample_runtime_compile PRIVATE spp_backend)
# The generation modules resolve host and runtime symbols from the executable.
set_target_properties(sample_runtime_compile PROPERTIES ENABLE_EXPORTS ON)
# The program reads wolf.spp from this folder at run time.
target_compile_definitions(sample_runtime_compile PRIVATE SAMPLE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

enable_testing()
add_test(NAME sample_runtime_compile COMMAND sample_runtime_compile)
