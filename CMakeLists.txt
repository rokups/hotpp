cmake_minimum_required(VERSION 3.20)
project(spp CXX)

# HOT_TOP_LEVEL is ON when S++ is the main project. It is OFF when a consumer project
# uses add_subdirectory. The options below default to ON only for the main project.
if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    set(HOT_TOP_LEVEL ON)
else()
    set(HOT_TOP_LEVEL OFF)
endif()
option(HOT_BUILD_TESTS "Build the S++ tests" ${HOT_TOP_LEVEL})
option(HOT_BUILD_SAMPLES "Build the S++ samples" ${HOT_TOP_LEVEL})
option(HOT_SAMPLES_STANDALONE_TEST "Configure, build and test samples/aot as a separate project" OFF)

# Put the build output in bin/ and lib/. Ninja Multi-Config adds /<CONFIG> to each path.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

enable_testing()

# The S++ backend, chosen at build time (design section 3.3).
#   CLANG_FILES: translate in memory, run an external clang++, dlopen the .so.
#   JIT:         translate and compile in memory with the Clang library, load with LLVM ORC.
set(HOT_BACKEND CLANG_FILES CACHE STRING "S++ backend: CLANG_FILES or JIT")
set_property(CACHE HOT_BACKEND PROPERTY STRINGS CLANG_FILES JIT)
if(NOT HOT_BACKEND STREQUAL "JIT" AND NOT HOT_BACKEND STREQUAL "CLANG_FILES")
    message(FATAL_ERROR "HOT_BACKEND must be CLANG_FILES or JIT. It is: ${HOT_BACKEND}")
endif()

# All scratch files go under one root. HOT_KEEP_TEMP keeps them for debugging.
option(HOT_KEEP_TEMP "Keep the scratch files under tmp/ for debugging" OFF)
set(HOT_SCRATCH_DIR ${CMAKE_BINARY_DIR}/tmp)

# The default standard is C++20. A parent project can set CMAKE_CXX_STANDARD to 17.
if(NOT CMAKE_CXX_STANDARD)
    set(CMAKE_CXX_STANDARD 20)
    # The scope of a parent project does not see the variable. hot_add_scripts in that scope
    # reads this property (hot_effective_cxx_standard), so it agrees with the build of sppc.
    set_property(GLOBAL PROPERTY HOT_DEFAULT_CXX_STANDARD 20)
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
# (HOT_DEFAULT_CXXFLAGS), so the generation modules use the same runtime.
# Use one build directory for each mode. The presets do this (see CMakePresets.json).

# Dependencies. CPM uses an installed package first and fetches only when the
# package is missing. libclang is not fetched: it must come from the system.
# -DCPM_USE_LOCAL_PACKAGES=OFF or -DCPM_DOWNLOAD_ALL=ON forces the fetch.
if(NOT DEFINED CPM_USE_LOCAL_PACKAGES)
    set(CPM_USE_LOCAL_PACKAGES ON)
endif()
include(cmake/CPM.cmake)
if(HOT_BUILD_TESTS)
    CPMAddPackage(
        NAME GTest
        GITHUB_REPOSITORY google/googletest
        GIT_TAG v1.17.0
        VERSION 1.17.0
        OPTIONS "INSTALL_GTEST OFF" "gtest_force_shared_crt ON"
    )
    include(cmake/HotTests.cmake)
endif()
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

# The Clang resource directory. The translator, the backends and the tests use it.
if(NOT MSVC)
    execute_process(COMMAND ${CMAKE_CXX_COMPILER} -print-resource-dir
                    OUTPUT_VARIABLE CLANG_RESOURCE_DIR OUTPUT_STRIP_TRAILING_WHITESPACE)
else()
    # unverified: not built on MSVC. MSVC has no -print-resource-dir. Set the cache variable
    # HOT_CLANG_RESOURCE_DIR to the resource directory of a Clang install.
    set(HOT_CLANG_RESOURCE_DIR "" CACHE PATH "The Clang resource directory (MSVC builds)")
    set(CLANG_RESOURCE_DIR "${HOT_CLANG_RESOURCE_DIR}")
endif()

# JitBackend (src/runtime): LLVM and Clang come from the system. CPM does not fetch them.
if(HOT_BACKEND STREQUAL "JIT")
    # LLVMConfig.cmake probes C headers (FindLibEdit), so the C language must be on.
    enable_language(C)
    find_package(LLVM REQUIRED CONFIG)
    find_package(Clang REQUIRED CONFIG)
    message(STATUS "JIT backend: LLVM ${LLVM_PACKAGE_VERSION}, Clang ${Clang_VERSION}")
endif()

# The compiler flags for the scripts. The backends use them.
# A list value uses "|" between the items.
separate_arguments(HOT_CXXFLAGS_LIST NATIVE_COMMAND "${CMAKE_CXX_FLAGS}")
list(JOIN HOT_CXXFLAGS_LIST "|" HOT_CXXFLAGS_JOINED)

# The headers that the precompiled and AOT scripts depend on (the engine headers are in tests/engine). The tests use them.
if(HOT_BUILD_TESTS)
    file(GLOB HOT_RUNTIME_HEADERS ${PROJECT_SOURCE_DIR}/include/hot/runtime/*.h)
    set(HOT_HEADERS ${PROJECT_SOURCE_DIR}/tests/engine/Actor.h ${PROJECT_SOURCE_DIR}/tests/engine/Math.h
                    ${PROJECT_SOURCE_DIR}/include/hot/runtime.h ${PROJECT_SOURCE_DIR}/include/hot/module.h
                    ${PROJECT_SOURCE_DIR}/include/hot/script.h
                    ${HOT_RUNTIME_HEADERS})
endif()

# The default include dirs of the backends. The tests use the engine
# headers, so a build with HOT_BUILD_TESTS adds them. A consumer
# build without this option passes its own dirs with DefaultBackendOptions.
# A list value uses "|" between the items.
set(HOT_DEFAULT_INCLUDE_DIRS ${PROJECT_SOURCE_DIR}/include)
if(HOT_BUILD_TESTS)
    list(APPEND HOT_DEFAULT_INCLUDE_DIRS ${PROJECT_SOURCE_DIR}/tests/engine)
endif()
list(JOIN HOT_DEFAULT_INCLUDE_DIRS "|" HOT_DEFAULT_INCLUDES_JOINED)

# The format library of the string interpolation: std::format or fmt (cmake/HotFormat.cmake).
include(cmake/HotFormat.cmake)
hot_format_library(HOT_FORMAT_LIB ${CMAKE_CXX_STANDARD})
if(HOT_FORMAT_LIB STREQUAL "fmt")
    hot_provide_fmt()
endif()
message(STATUS "String interpolation uses: ${HOT_FORMAT_LIB}")

include(cmake/HotScripts.cmake)

# The order follows the dependencies: the libraries, then the tests.
add_subdirectory(src)

# GoogleTest targets. They need the pre_* targets and sppc from above.
if(HOT_BUILD_TESTS)
    add_subdirectory(tests)
    # The benchmark uses the test engine and the test scripts.
    add_subdirectory(tools/bench)
    add_subdirectory(editors/vscode)
endif()

if(HOT_BUILD_SAMPLES)
    add_subdirectory(samples)
endif()
