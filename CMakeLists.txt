cmake_minimum_required(VERSION 3.20)
project(spp CXX)

# Put the build output in bin/ and lib/. Ninja Multi-Config adds /<CONFIG> to each path.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

enable_testing()

# The S++ backend, chosen at build time (design section 3.3).
#   CLANG_FILES: translate in memory, run an external clang++, dlopen the .so.
#   JIT:         translate and compile in memory with the Clang library, load with LLVM ORC.
set(SPP_BACKEND CLANG_FILES CACHE STRING "S++ backend: CLANG_FILES or JIT")
set_property(CACHE SPP_BACKEND PROPERTY STRINGS CLANG_FILES JIT)
if(NOT SPP_BACKEND STREQUAL "JIT" AND NOT SPP_BACKEND STREQUAL "CLANG_FILES")
    message(FATAL_ERROR "SPP_BACKEND must be CLANG_FILES or JIT. It is: ${SPP_BACKEND}")
endif()

# All scratch files go under one root. SPP_KEEP_TEMP keeps them for debugging.
option(SPP_KEEP_TEMP "Keep the scratch files under tmp/ for debugging" OFF)
set(SPP_SCRATCH_DIR ${CMAKE_BINARY_DIR}/tmp)

# The default standard is C++20. A parent project can set CMAKE_CXX_STANDARD to 17.
if(NOT CMAKE_CXX_STANDARD)
    set(CMAKE_CXX_STANDARD 20)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
add_compile_options(-Wall -Wextra)
if(MSVC)
    # unverified: not built on MSVC. /JMC (Just My Code) helps the debugger skip the library code.
    add_compile_options($<$<CONFIG:Debug>:/JMC>)
endif()

# Sanitizers. Use -DCMAKE_CXX_FLAGS=-fsanitize=address or -fsanitize=thread.
# CMake passes the flags to the compile and to the link of every target.
# The script backends pass the same flags to the compile of the scripts
# (SPP_DEFAULT_CXXFLAGS), so the generation modules use the same runtime.
# Use one build directory for each mode. The presets do this (see CMakePresets.json).

# Dependencies. CPM uses an installed package first and fetches only when the
# package is missing. libclang is not fetched: it must come from the system.
# -DCPM_USE_LOCAL_PACKAGES=OFF or -DCPM_DOWNLOAD_ALL=ON forces the fetch.
if(NOT DEFINED CPM_USE_LOCAL_PACKAGES)
    set(CPM_USE_LOCAL_PACKAGES ON)
endif()
include(cmake/CPM.cmake)
CPMAddPackage(
    NAME GTest
    GITHUB_REPOSITORY google/googletest
    GIT_TAG v1.17.0
    VERSION 1.17.0
    OPTIONS "INSTALL_GTEST OFF" "gtest_force_shared_crt ON"
)
include(cmake/SppTests.cmake)
# lexy: header-only parser library. Only src/translator/parser/ includes its headers.
# Use only foonathan::lexy::core. Do not link foonathan::lexy (it builds lexy_file).
CPMAddPackage(
    NAME lexy
    GITHUB_REPOSITORY foonathan/lexy
    GIT_TAG v2025.05.0
    VERSION 2025.05.0
    OPTIONS "LEXY_BUILD_EXAMPLES OFF" "LEXY_BUILD_TESTS OFF" "LEXY_BUILD_PACKAGE OFF" "LEXY_ENABLE_INSTALL OFF"
)

# nlohmann_json: the language server in src/ls uses it.
CPMAddPackage(
    NAME nlohmann_json
    VERSION 3.12.0
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
    OPTIONS "JSON_BuildTests OFF" "JSON_Install OFF"
)

# The script runtime and the language server use threads.
find_package(Threads REQUIRED)

# The Clang resource directory. The translator, the backends, the examples and the tests use it.
if(NOT MSVC)
    execute_process(COMMAND ${CMAKE_CXX_COMPILER} -print-resource-dir
                    OUTPUT_VARIABLE CLANG_RESOURCE_DIR OUTPUT_STRIP_TRAILING_WHITESPACE)
else()
    # unverified: not built on MSVC. MSVC has no -print-resource-dir. Set the cache variable
    # SPP_CLANG_RESOURCE_DIR to the resource directory of a Clang install.
    set(SPP_CLANG_RESOURCE_DIR "" CACHE PATH "The Clang resource directory (MSVC builds)")
    set(CLANG_RESOURCE_DIR "${SPP_CLANG_RESOURCE_DIR}")
endif()

# JitBackend (src/runtime): LLVM and Clang come from the system. CPM does not fetch them.
if(SPP_BACKEND STREQUAL "JIT")
    # LLVMConfig.cmake probes C headers (FindLibEdit), so the C language must be on.
    enable_language(C)
    find_package(LLVM REQUIRED CONFIG)
    find_package(Clang REQUIRED CONFIG)
    message(STATUS "JIT backend: LLVM ${LLVM_PACKAGE_VERSION}, Clang ${Clang_VERSION}")
endif()

# The compiler flags for the scripts. The backends and the examples use them.
# A list value uses "|" between the items.
separate_arguments(SPP_CXXFLAGS_LIST NATIVE_COMMAND "${CMAKE_CXX_FLAGS}")
list(JOIN SPP_CXXFLAGS_LIST "|" SPP_CXXFLAGS_JOINED)

# The headers that the precompiled and AOT scripts depend on. The examples and the tests use them.
file(GLOB SPP_RUNTIME_HEADERS ${CMAKE_SOURCE_DIR}/include/spp/runtime/*.h)
set(SPP_HEADERS ${CMAKE_SOURCE_DIR}/example/engine/Actor.h ${CMAKE_SOURCE_DIR}/example/engine/Math.h
                ${CMAKE_SOURCE_DIR}/include/spp/runtime.h ${CMAKE_SOURCE_DIR}/include/spp/module.h
                ${SPP_RUNTIME_HEADERS})

# The format library of the string interpolation: std::format or fmt (cmake/SppFormat.cmake).
include(cmake/SppFormat.cmake)
spp_format_library(SPP_FORMAT_LIB ${CMAKE_CXX_STANDARD})
if(SPP_FORMAT_LIB STREQUAL "fmt")
    spp_provide_fmt()
endif()
message(STATUS "String interpolation uses: ${SPP_FORMAT_LIB}")

include(cmake/SppScripts.cmake)

# The order follows the dependencies: the libraries, the examples, then the tests.
add_subdirectory(src)
add_subdirectory(example)

# GoogleTest targets. They need the pre_* targets and sppc from above.
add_subdirectory(tests)
add_subdirectory(editors/vscode)
