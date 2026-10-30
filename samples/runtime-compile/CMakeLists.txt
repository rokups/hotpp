# The sample runtime-compile: a program that compiles a script at run time and loads it.
# This file works in the S++ build (add_subdirectory) and as a separate project.
cmake_minimum_required(VERSION 3.20)
project(spp_sample_runtime_compile CXX)

# The S++ source tree. A separate build uses it. The S++ build already has the targets.
set(SPP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH "The S++ source tree")
if(NOT TARGET sppc)
    add_subdirectory(${SPP_ROOT} spp)
endif()

# One program source, two script variants. The extension picks the script.
#   sample_runtime_compile_spp reads script/wolf.spp.
#   sample_runtime_compile_cpp reads script/wolf.cpp.
# spp_backend has make_default_backend and the loader (spp_runtime).
foreach(variant spp cpp)
    set(target sample_runtime_compile_${variant})
    add_executable(${target} main.cpp)
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(${target} PRIVATE spp_backend)
    # The generation modules resolve host and runtime symbols from the executable.
    set_target_properties(${target} PROPERTIES ENABLE_EXPORTS ON)
    # SAMPLE_DIR has Creature.h (an include dir of the compile at run time).
    # SAMPLE_SCRIPT_DIR and SAMPLE_SCRIPT_EXT select the script that the program reads at run time.
    target_compile_definitions(${target} PRIVATE
        SAMPLE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
        SAMPLE_SCRIPT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/script"
        SAMPLE_SCRIPT_EXT=".${variant}")

    add_test(NAME ${target} COMMAND ${target})
endforeach()

# The normal toolchain compiles the .cpp script too. A .cpp script must compile unchanged.
add_library(runtime_compile_cpp_scripts_plain OBJECT script/wolf.cpp)
target_include_directories(runtime_compile_cpp_scripts_plain PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(runtime_compile_cpp_scripts_plain PRIVATE cxx_std_17)

enable_testing()
