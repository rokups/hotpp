# The sample hot-reload: a program that replaces the code of a live script object.
# This file works in the Hot++ build (add_subdirectory) and as a separate project.
cmake_minimum_required(VERSION 3.20)
project(hot_sample_hot_reload CXX)

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

if(NOT TARGET hot::backend)
    message(STATUS "sample_hot_reload: skipped (this Hot++ has no target hot::backend)")
    return()
endif()

# One program source, two script variants. The extension picks the
# scripts. Both variants are in the folder script. The names of the script files stay wolf_v1 and wolf_v2.
#   sample_hot_reload_hot reads script/wolf_v1.hot and wolf_v2.hot.
#   sample_hot_reload_cpp reads script/wolf_v1.cpp and wolf_v2.cpp.
foreach(variant hot cpp)
    # The variant "hot" uses the extension ".hot".
    set(ext ${variant})
    if(variant STREQUAL "hot")
        set(ext hot)
    endif()
    set(target sample_hot_reload_${variant})
    add_executable(${target} main.cpp)
    # hot_add_host links hot::backend (make_default_backend and the loader) and exports the symbols
    # that the generation modules need. HOT_SCRIPT_DIR is the folder script. HOT_SAMPLE_EXT is the
    # extension of the variant (.hot or .cpp).
    target_compile_definitions(${target} PRIVATE HOT_SAMPLE_EXT=".${ext}")
    hot_add_host(TARGET ${target} MODE HOT_RELOAD
                 SCRIPTS script/wolf_v1.${ext} script/wolf_v2.${ext}
                 INCLUDES ${CMAKE_CURRENT_SOURCE_DIR} DEPENDS Creature.h)

    add_test(NAME ${target} COMMAND ${target} ${CMAKE_CURRENT_SOURCE_DIR}/script)
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
