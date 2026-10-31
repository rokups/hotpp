# The sample jit-backend: the program builds JitBackend itself. The backend compiles in memory
# with the Clang library. It needs HOT_ENABLE_JIT=ON (or AUTO with the packages) and the LLVM and Clang development packages.
# This file works in the Hot++ build (add_subdirectory) and as a separate project.
cmake_minimum_required(VERSION 3.20)
project(hot_sample_jit CXX)

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

if(NOT TARGET hot::jit_backend)
    message(STATUS "sample_jit: skipped (this Hot++ has no JIT backend, see HOT_ENABLE_JIT)")
    return()
endif()

# One program source, two script variants. The extension picks the script.
#   sample_jit_hot reads script/wolf.hot.
#   sample_jit_cpp reads script/wolf.cpp.
foreach(variant hot cpp)
    # The variant "hot" uses the extension ".hot".
    set(ext ${variant})
    if(variant STREQUAL "hot")
        set(ext hot)
    endif()
    set(target sample_jit_${variant})
    add_executable(${target} main.cpp)
    # hot_add_host sets ENABLE_EXPORTS (the generation modules resolve host and runtime symbols
    # from the executable) and the define HOT_SCRIPT_DIR (the folder script, not in a portable
    # build). HOT_SAMPLE_EXT is the extension of the variant.
    hot_add_host(TARGET ${target} MODE HOT_RELOAD SCRIPTS script/wolf.${ext}
                 INCLUDES ${CMAKE_CURRENT_SOURCE_DIR} DEPENDS Creature.h)
    target_link_libraries(${target} PRIVATE hot::jit_backend)
    target_compile_definitions(${target} PRIVATE HOT_SAMPLE_EXT=".${ext}")

    add_test(NAME ${target} COMMAND ${target} ${CMAKE_CURRENT_SOURCE_DIR}/script)
endforeach()

# The normal toolchain compiles the .cpp script too. A .cpp script must compile unchanged.
add_library(jit_backend_cpp_scripts_plain OBJECT script/wolf.cpp)
target_include_directories(jit_backend_cpp_scripts_plain PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(jit_backend_cpp_scripts_plain PRIVATE cxx_std_17)

enable_testing()
