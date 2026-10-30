# The sample clang-files-backend: the program builds ClangFilesBackend itself.
# HOT_BACKEND=CLANG_FILES selects this backend for make_default_backend. A program that builds it
# sets the compiler, the flags and the work folder itself.
# This file works in the S++ build (add_subdirectory) and as a separate project.
cmake_minimum_required(VERSION 3.20)
project(hot_sample_clang_files_backend CXX)

# The S++ source tree. A separate build uses it. The S++ build already has the targets.
set(HOT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH "The S++ source tree")
if(NOT TARGET hotc)
    add_subdirectory(${HOT_ROOT} hot)
endif()

# The compiler needs its resource folder to find the compiler headers.
execute_process(COMMAND ${CMAKE_CXX_COMPILER} -print-resource-dir
                OUTPUT_VARIABLE resource_dir OUTPUT_STRIP_TRAILING_WHITESPACE)

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
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(${target} PRIVATE hot_clang_backend)
    # The generation modules resolve host and runtime symbols from the executable.
    set_target_properties(${target} PROPERTIES ENABLE_EXPORTS ON)
    # SAMPLE_DIR has Creature.h. SAMPLE_SCRIPT_DIR and SAMPLE_SCRIPT_EXT select the script
    # that the program reads at run time.
    target_compile_definitions(${target} PRIVATE
        SAMPLE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
        SAMPLE_SCRIPT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/script"
        SAMPLE_SCRIPT_EXT=".${ext}"
        SAMPLE_HOT_INCLUDE_DIR="${HOT_INCLUDE_DIR}"
        SAMPLE_CXX="${CMAKE_CXX_COMPILER}"
        SAMPLE_CXXFLAGS="${CMAKE_CXX_FLAGS}"
        SAMPLE_RESOURCE_DIR="${resource_dir}"
        SAMPLE_WORK_DIR="${CMAKE_CURRENT_BINARY_DIR}/hot_work_${variant}")

    add_test(NAME ${target} COMMAND ${target})
endforeach()

# The normal toolchain compiles the .cpp script too. A .cpp script must compile unchanged.
add_library(clang_files_backend_cpp_scripts_plain OBJECT script/wolf.cpp)
target_include_directories(clang_files_backend_cpp_scripts_plain PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(clang_files_backend_cpp_scripts_plain PRIVATE cxx_std_17)

enable_testing()
