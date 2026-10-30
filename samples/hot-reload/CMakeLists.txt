# The sample hot-reload: a program that replaces the code of a live script object.
# This file works in the S++ build (add_subdirectory) and as a separate project.
cmake_minimum_required(VERSION 3.20)
project(spp_sample_hot_reload CXX)

# The S++ source tree. A separate build uses it. The S++ build already has the targets.
set(SPP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH "The S++ source tree")
if(NOT TARGET sppc)
    add_subdirectory(${SPP_ROOT} spp)
endif()

# One program source, two script variants. The extension picks the
# scripts. Both variants are in the folder script. The names of the script files stay wolf_v1 and wolf_v2.
#   sample_hot_reload_spp reads script/wolf_v1.spp and wolf_v2.spp.
#   sample_hot_reload_cpp reads script/wolf_v1.cpp and wolf_v2.cpp.
# spp_backend has make_default_backend and the loader (spp_runtime).
foreach(variant spp cpp)
    set(target sample_hot_reload_${variant})
    add_executable(${target} main.cpp)
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(${target} PRIVATE spp_backend)
    # The generation modules resolve host and runtime symbols from the executable.
    set_target_properties(${target} PROPERTIES ENABLE_EXPORTS ON)
    # SAMPLE_DIR has Creature.h (an include dir of the compile at run time).
    # SAMPLE_SCRIPT_DIR and SAMPLE_SCRIPT_EXT select the scripts that the program reads at run time.
    target_compile_definitions(${target} PRIVATE
        SAMPLE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
        SAMPLE_SCRIPT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/script"
        SAMPLE_SCRIPT_EXT=".${variant}")

    add_test(NAME ${target} COMMAND ${target})
    # The output must have the lines of both generations.
    set_tests_properties(${target} PROPERTIES
        PASS_REGULAR_EXPRESSION "\\[v1\\][^\n]*\n[^\n]*\\[v1\\][^\n]*\n[^\n]*\\[v2\\][^\n]*\n[^\n]*\\[v2\\]")
endforeach()

# The normal toolchain compiles the .cpp scripts too. A .cpp script must compile unchanged.
add_library(hot_reload_cpp_scripts_plain OBJECT script/wolf_v1.cpp script/wolf_v2.cpp)
target_include_directories(hot_reload_cpp_scripts_plain PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(hot_reload_cpp_scripts_plain PRIVATE cxx_std_17)

enable_testing()
