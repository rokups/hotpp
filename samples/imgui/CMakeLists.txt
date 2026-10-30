# The sample imgui: a Dear ImGui window whose user interface a script draws.
# The program builds ClangFilesBackend itself and reloads the script when the file changes.
# It needs SDL3. Without SDL3 the configure prints a message and adds no target.
# This file works in the S++ build (add_subdirectory) and as a separate project.
cmake_minimum_required(VERSION 3.24)
project(spp_sample_imgui CXX)

# The S++ source tree. A separate build uses it. The S++ build already has the targets.
set(SPP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH "The S++ source tree")
if(NOT TARGET sppc)
    add_subdirectory(${SPP_ROOT} spp)
endif()

find_package(SDL3 QUIET)
if(NOT SDL3_FOUND)
    message(STATUS "sample_imgui: skipped (SDL3 not found)")
    return()
endif()

# CPM fetches Dear ImGui. The S++ build may not load CPM, so load it here when needed.
if(NOT COMMAND CPMAddPackage)
    include(${SPP_ROOT}/cmake/CPM.cmake)
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

# The compiler needs its resource folder to find the compiler headers.
execute_process(COMMAND ${CMAKE_CXX_COMPILER} -print-resource-dir
                OUTPUT_VARIABLE resource_dir OUTPUT_STRIP_TRAILING_WHITESPACE)
# A list value uses "|" between the items.
separate_arguments(cxxflags_list NATIVE_COMMAND "${CMAKE_CXX_FLAGS}")
list(JOIN cxxflags_list "|" cxxflags_joined)

# One program source, two script variants. The extension picks the script.
#   sample_imgui_spp reads script/demo.spp.
#   sample_imgui_cpp reads script/demo.cpp.
# The host builds ClangFilesOptions itself. It does not use make_default_backend().
foreach(variant spp cpp)
    set(target sample_imgui_${variant})
    add_executable(${target} main.cpp UiApp.cpp)
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    # Whole archive: a script can call any ImGui function. The linker must keep all objects.
    target_link_libraries(${target} PRIVATE spp_runtime spp_clang_backend "$<LINK_LIBRARY:WHOLE_ARCHIVE,imgui_sdl3>")
    # The generation modules resolve host and runtime symbols from the executable.
    set_target_properties(${target} PROPERTIES ENABLE_EXPORTS ON)
    # SAMPLE_SCRIPT_DIR and SAMPLE_SCRIPT_EXT select the default script of the program.
    target_compile_definitions(${target} PRIVATE
        IMGUI_APP_CXX="${CMAKE_CXX_COMPILER}"
        IMGUI_APP_CXXFLAGS="${cxxflags_joined}"
        IMGUI_APP_INCLUDES="${SPP_INCLUDE_DIR}|${CMAKE_CURRENT_SOURCE_DIR}|${imgui_SOURCE_DIR}"
        IMGUI_APP_RESOURCE_DIR="${resource_dir}"
        IMGUI_APP_WORK_DIR="${CMAKE_CURRENT_BINARY_DIR}/spp_work"
        SAMPLE_SCRIPT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/script"
        SAMPLE_SCRIPT_EXT=".${variant}")
endforeach()

# The normal toolchain compiles the .cpp script too. The compile uses no sppc and no spp include dir.
add_library(imgui_cpp_scripts_plain OBJECT script/demo.cpp)
target_include_directories(imgui_cpp_scripts_plain PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(imgui_cpp_scripts_plain PRIVATE imgui_sdl3)
target_compile_features(imgui_cpp_scripts_plain PRIVATE cxx_std_17)

# The language server reads this file.
# The language server serves the .spp scripts only.
spp_script_host(NAME sample_imgui TARGET sample_imgui_spp SCRIPT_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/script)

# No test: the program opens a window.
