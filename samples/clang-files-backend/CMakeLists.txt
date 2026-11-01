# Copyright (c) 2026 the Hot++ project.
# This work is licensed under the terms of the MIT license.
# For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

# The sample clang-files-backend: the program builds ClangFilesBackend itself.
# HOT_ENABLE_CLANG_FILES=ON builds this backend into hot::backend (make_default_backend).
# A program that builds it itself sets the compiler, the flags and the work folder.
# This file works in the Hot++ build (add_subdirectory) and as a separate project.
cmake_minimum_required(VERSION 3.20)
project(hot_sample_clang_files_backend CXX)

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

if(NOT TARGET hot::clang_backend)
    message(STATUS "sample_clang_files: skipped (this Hot++ has no target hot::clang_backend)")
    return()
endif()

# One program source, two script variants. The extension picks the script.
#   sample_clang_files_hot reads script/wolf.hot.
#   sample_clang_files_cpp reads script/wolf.cpp.
foreach(variant hot cpp)
    # The variant "hot" uses the extension ".hot".
    set(ext ${variant})
    if(variant STREQUAL "hot")
        set(ext hot)
    endif()
    set(target sample_clang_files_${variant})
    add_executable(${target} main.cpp)
    # hot_add_host sets ENABLE_EXPORTS (the generation modules resolve host and runtime symbols
    # from the executable) and the define HOT_SCRIPT_DIR (the folder script, not in a portable
    # build). HOT_SAMPLE_EXT is the extension of the variant.
    hot_add_host(TARGET ${target} MODE HOT_RELOAD SCRIPTS script/wolf.${ext}
                 INCLUDES ${CMAKE_CURRENT_SOURCE_DIR} DEPENDS Creature.h)
    target_link_libraries(${target} PRIVATE hot::clang_backend)
    target_compile_definitions(${target} PRIVATE HOT_SAMPLE_EXT=".${ext}")

    add_test(NAME ${target} COMMAND ${target} ${CMAKE_CURRENT_SOURCE_DIR}/script)
endforeach()

# The normal toolchain compiles the .cpp script too. A .cpp script must compile unchanged.
add_library(clang_files_backend_cpp_scripts_plain OBJECT script/wolf.cpp)
target_include_directories(clang_files_backend_cpp_scripts_plain PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(clang_files_backend_cpp_scripts_plain PRIVATE cxx_std_17)

enable_testing()
