# Copyright (c) 2026 the Hot++ project.
# This work is licensed under the terms of the MIT license.
# For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

# The sample imgui: a Dear ImGui window whose user interface a script draws.
# The program builds ClangFilesBackend itself and reloads the script when the file changes.
# It needs SDL3. Without SDL3 the configure prints a message and adds no target.
# This file works in the Hot++ build (add_subdirectory) and as a separate project.
cmake_minimum_required(VERSION 3.24)
project(hot_sample_imgui CXX)

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
    message(STATUS "sample_imgui: skipped (this Hot++ has no target hot::clang_backend)")
    return()
endif()

find_package(SDL3 QUIET)
if(NOT SDL3_FOUND)
    message(STATUS "sample_imgui: skipped (SDL3 not found)")
    return()
endif()

# CPM fetches Dear ImGui. The Hot++ build may not load CPM, so load it here when needed.
if(NOT COMMAND CPMAddPackage)
    include(${HOT_ROOT}/cmake/CPM.cmake)
endif()
CPMAddPackage(NAME imgui GITHUB_REPOSITORY ocornut/imgui GIT_TAG v1.92.9b DOWNLOAD_ONLY YES)
add_library(imgui_sdl3 STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer3.cpp)
target_include_directories(imgui_sdl3 PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(imgui_sdl3 PUBLIC SDL3::SDL3)

# One program source, two script variants. The extension picks the script.
#   sample_imgui_hot reads script/demo.hot.
#   sample_imgui_cpp reads script/demo.cpp.
# The host builds ClangFilesOptions itself. It does not use make_default_backend().
foreach(variant hot cpp)
    # The variant "hot" uses the extension ".hot".
    set(ext ${variant})
    if(variant STREQUAL "hot")
        set(ext hot)
    endif()
    set(target sample_imgui_${variant})
    add_executable(${target} main.cpp UiApp.cpp)
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    # Whole archive: a script can call any ImGui function. The linker must keep all objects.
    target_link_libraries(${target} PRIVATE hot::runtime hot::clang_backend "$<LINK_LIBRARY:WHOLE_ARCHIVE,imgui_sdl3>")
    # The generation modules resolve host and runtime symbols from the executable.
    set_target_properties(${target} PROPERTIES ENABLE_EXPORTS ON)
    hot_export_host(${target})
    # HOT_SAMPLE_EXT is the extension of the variant. The paths of the build go in only in a
    # development build. A portable build gets the script from argv[1] and the include folder
    # of ImGui from the environment variable HOT_IMGUI_INCLUDE_DIR.
    target_compile_definitions(${target} PRIVATE HOT_SAMPLE_EXT=".${ext}")
    if(NOT HOT_PORTABLE)
        target_compile_definitions(${target} PRIVATE
            HOT_SCRIPT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/script"
            HOT_IMGUI_INCLUDE_DIR="${imgui_SOURCE_DIR}")
    endif()
endforeach()

# The normal toolchain compiles the .cpp script too. A .cpp script must compile unchanged.
add_library(imgui_cpp_scripts_plain OBJECT script/demo.cpp)
target_include_directories(imgui_cpp_scripts_plain PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR})
# UiApp.h includes hot/runtime/export.h (HOT_EXPORT). hot::runtime_core gives the include directory.
target_link_libraries(imgui_cpp_scripts_plain PRIVATE imgui_sdl3 hot::runtime_core)
target_compile_features(imgui_cpp_scripts_plain PRIVATE cxx_std_17)

# The language server reads this file.
# The language server serves the .hot scripts only.
hot_script_host(NAME sample_imgui TARGET sample_imgui_hot SCRIPT_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/script)

# No test: the program opens a window.
