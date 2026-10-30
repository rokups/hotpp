# The sample jit-backend: the program builds JitBackend itself. The backend compiles in memory
# with the Clang library. It needs SPP_BACKEND=JIT and the LLVM and Clang development packages.
# This file works in the S++ build (add_subdirectory) and as a separate project.
cmake_minimum_required(VERSION 3.20)
project(spp_sample_jit CXX)

# The S++ build sets SPP_BACKEND. A separate project asks for the JIT backend by default.
if(NOT DEFINED SPP_BACKEND)
    set(SPP_BACKEND JIT CACHE STRING "S++ backend: CLANG_FILES or JIT")
endif()

# The S++ source tree. A separate build uses it. The S++ build already has the targets.
set(SPP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH "The S++ source tree")
if(NOT TARGET sppc)
    add_subdirectory(${SPP_ROOT} spp)
endif()

if(NOT SPP_BACKEND STREQUAL "JIT")
    message(STATUS "sample_jit: skipped, SPP_BACKEND is ${SPP_BACKEND}")
    return()
endif()

# The compiler needs its resource folder to find the compiler headers.
execute_process(COMMAND ${CMAKE_CXX_COMPILER} -print-resource-dir
                OUTPUT_VARIABLE resource_dir OUTPUT_STRIP_TRAILING_WHITESPACE)

# One program source, two script variants. The extension picks the script.
#   sample_jit_spp reads script/wolf.spp.
#   sample_jit_cpp reads script/wolf.cpp.
foreach(variant spp cpp)
    set(target sample_jit_${variant})
    add_executable(${target} main.cpp)
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(${target} PRIVATE spp_jit_backend)
    # The generation modules resolve host and runtime symbols from the executable.
    set_target_properties(${target} PROPERTIES ENABLE_EXPORTS ON)
    # SAMPLE_DIR has Creature.h. SAMPLE_SCRIPT_DIR and SAMPLE_SCRIPT_EXT select the script
    # that the program reads at run time.
    target_compile_definitions(${target} PRIVATE
        SAMPLE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
        SAMPLE_SCRIPT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/script"
        SAMPLE_SCRIPT_EXT=".${variant}"
        SAMPLE_SPP_INCLUDE_DIR="${SPP_INCLUDE_DIR}"
        SAMPLE_CXX="${CMAKE_CXX_COMPILER}"
        SAMPLE_CXXFLAGS="${CMAKE_CXX_FLAGS}"
        SAMPLE_RESOURCE_DIR="${resource_dir}")

    add_test(NAME ${target} COMMAND ${target})
endforeach()

# The normal toolchain compiles the .cpp script too. A .cpp script must compile unchanged.
add_library(jit_backend_cpp_scripts_plain OBJECT script/wolf.cpp)
target_include_directories(jit_backend_cpp_scripts_plain PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(jit_backend_cpp_scripts_plain PRIVATE cxx_std_17)

enable_testing()
