# Copyright (c) 2026 the Hot++ project.
# This work is licensed under the terms of the MIT license.
# For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

# The sample aot: a program with the C++ of a script inside the binary.
# This file works in the Hot++ build (add_subdirectory) and as a separate project.
cmake_minimum_required(VERSION 3.20)
project(hot_sample_aot CXX)

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

# One program source, two script variants, and two builds of each variant. The extension picks the
# script. Both variants are in the folder script. Each build has its own AOT library and its own program.
#   sample_aot_hot has the C++ that hotc makes from script/den.hot and script/wolf.hot.
#   sample_aot_cpp has the C++ that hotc makes from script/wolf.cpp.
#   sample_aot_hot_opt and sample_aot_cpp_opt are the same scripts as optimized AOT code (OPTIMIZED).
#   An optimized build is faster. It cannot override or reload a script.
# The scripts of one hot_add_host call must have different stems, so each variant has its own call.
# The library sample_aot_<name>_scripts has the result. The program links hot::aot.
enable_testing()
foreach(variant hot cpp)
    foreach(suffix "" _opt)
        set(optimized "")
        if(suffix STREQUAL "_opt")
            set(optimized OPTIMIZED)
        endif()
        set(target sample_aot_${variant}${suffix})
        set(scripts script/wolf.${variant})
        if(variant STREQUAL "hot")
            set(scripts script/den.hot script/wolf.hot) # two units: wolf.hot imports den.hot
        endif()
        add_executable(${target} main.cpp)
        hot_add_host(TARGET ${target} MODE AOT ${optimized} SCRIPTS ${scripts}
                     INCLUDES ${CMAKE_CURRENT_SOURCE_DIR} DEPENDS Creature.h)
        add_test(NAME ${target} COMMAND ${target})
    endforeach()
endforeach()

# The normal toolchain compiles the .cpp script too. A .cpp script must compile unchanged.
add_library(aot_cpp_scripts_plain OBJECT script/wolf.cpp)
target_include_directories(aot_cpp_scripts_plain PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(aot_cpp_scripts_plain PRIVATE cxx_std_17)
