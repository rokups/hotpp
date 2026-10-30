# The sample aot: a program with the C++ of a script inside the binary.
# This file works in the Hot++ build (add_subdirectory) and as a separate project.
cmake_minimum_required(VERSION 3.20)
project(hot_sample_aot CXX)

# The Hot++ source tree. A separate build uses it. The Hot++ build already has the targets.
set(HOT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH "The Hot++ source tree")
if(NOT TARGET hotc)
    add_subdirectory(${HOT_ROOT} hot)
endif()

# One program source, two script variants. The extension picks the script. Both variants are
# in the folder script. Each variant has its own AOT library and its own program.
#   sample_aot_hot has the C++ that hotc makes from script/wolf.hot.
#   sample_aot_cpp has the C++ that hotc makes from script/wolf.cpp.
# A call of hot_add_host takes scripts with one stem only, so each variant has its own call.
# The library sample_aot_<variant>_scripts has the result and links hot::aot.
enable_testing()
foreach(variant hot cpp)
    # The variant "hot" uses the extension ".hot".
    set(ext ${variant})
    if(variant STREQUAL "hot")
        set(ext hot)
    endif()
    set(target sample_aot_${variant})
    add_executable(${target} main.cpp)
    hot_add_host(TARGET ${target} MODE AOT SCRIPTS script/wolf.${ext}
                 INCLUDES ${CMAKE_CURRENT_SOURCE_DIR} DEPENDS Creature.h)
    add_test(NAME ${target} COMMAND ${target})
endforeach()

# The normal toolchain compiles the .cpp script too. A .cpp script must compile unchanged.
add_library(aot_cpp_scripts_plain OBJECT script/wolf.cpp)
target_include_directories(aot_cpp_scripts_plain PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(aot_cpp_scripts_plain PRIVATE cxx_std_17)
