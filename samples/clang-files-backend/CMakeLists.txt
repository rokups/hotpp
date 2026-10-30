# The sample clang-files-backend: the program builds ClangFilesBackend itself.
# SPP_BACKEND=CLANG_FILES selects this backend for make_default_backend. A program that builds it
# sets the compiler, the flags and the work folder itself.
# This file works in the S++ build (add_subdirectory) and as a separate project.
cmake_minimum_required(VERSION 3.20)
project(spp_sample_clang_files_backend CXX)

# The S++ source tree. A separate build uses it. The S++ build already has the targets.
set(SPP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH "The S++ source tree")
if(NOT TARGET sppc)
    add_subdirectory(${SPP_ROOT} spp)
endif()

# The compiler needs its resource folder to find the compiler headers.
execute_process(COMMAND ${CMAKE_CXX_COMPILER} -print-resource-dir
                OUTPUT_VARIABLE resource_dir OUTPUT_STRIP_TRAILING_WHITESPACE)

add_executable(sample_clang_files main.cpp)
target_include_directories(sample_clang_files PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(sample_clang_files PRIVATE spp_clang_backend)
# The generation modules resolve host and runtime symbols from the executable.
set_target_properties(sample_clang_files PROPERTIES ENABLE_EXPORTS ON)
target_compile_definitions(sample_clang_files PRIVATE
    SAMPLE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    SAMPLE_SPP_INCLUDE_DIR="${SPP_INCLUDE_DIR}"
    SAMPLE_CXX="${CMAKE_CXX_COMPILER}"
    SAMPLE_CXXFLAGS="${CMAKE_CXX_FLAGS}"
    SAMPLE_RESOURCE_DIR="${resource_dir}"
    SAMPLE_WORK_DIR="${CMAKE_CURRENT_BINARY_DIR}/spp_work")

enable_testing()
add_test(NAME sample_clang_files COMMAND sample_clang_files)
