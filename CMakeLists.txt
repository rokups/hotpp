# Copyright (c) 2026 the Hot++ project.
# This work is licensed under the terms of the MIT license.
# For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

cmake_minimum_required(VERSION 3.20)
project(hotpp VERSION 0.1.0 LANGUAGES CXX)

include(GNUInstallDirs)

# HOT_TOP_LEVEL is ON when Hot++ is the main project. It is OFF when a consumer project
# uses add_subdirectory. The options below default to ON only for the main project.
if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    set(HOT_TOP_LEVEL ON)
else()
    set(HOT_TOP_LEVEL OFF)
endif()
option(HOT_INSTALL "Generate the install rules of the Hot++ SDK" ${HOT_TOP_LEVEL})
option(HOT_BUILD_TESTS "Build the Hot++ tests" ${HOT_TOP_LEVEL})
option(HOT_BUILD_SAMPLES "Build the Hot++ samples" ${HOT_TOP_LEVEL})
option(HOT_BUILD_LS "Build the language server hot-ls (needs HOT_BUILD_TRANSLATOR)" ON)
option(HOT_BUILD_VSCODE "Add the VS Code extension: the package, the install rules and the node tests" ON)
option(HOT_SAMPLES_STANDALONE_TEST "Configure, build and test samples/aot as a separate project" OFF)
# The translator (hotc_lib, hotc, hot-ls) and the run-time backends need libclang. A cross build
# has no libclang for the target, so the translator is off by default. Use HOT_HOST_HOTC to give
# the build a hotc that runs on the build host. A cross build to Windows can use the LLVM release
# for Windows (HOT_LLVM_DIR, HOT_LLVM_RELEASES_DIR or HOT_DOWNLOAD_LLVM, cmake/HotDownloadLlvm.cmake). Then the
# translator is on by default, and the emulator (wine) runs hotc.exe and the tests.
include(cmake/HotLlvmVersion.cmake)
option(HOT_DOWNLOAD_LLVM "Download the LLVM release for Windows (libclang.dll, clang++.exe)" OFF)
set(HOT_LLVM_DIR "" CACHE PATH "The folder of an unpacked LLVM release for Windows (no download)")
# A folder with unpacked releases. When HOT_LLVM_DIR is empty, the release folder of
# HOT_LLVM_VERSION in it is HOT_LLVM_DIR. Thus a preset does not repeat the version.
set(HOT_LLVM_RELEASES_DIR "" CACHE PATH "A folder with unpacked LLVM releases for Windows")
if(NOT HOT_LLVM_DIR AND HOT_LLVM_RELEASES_DIR)
    set(HOT_LLVM_DIR "${HOT_LLVM_RELEASES_DIR}/${HOT_LLVM_RELEASE_NAME}")
endif()
if(CMAKE_CROSSCOMPILING AND NOT HOT_LLVM_DIR AND NOT HOT_DOWNLOAD_LLVM)
    set(HOT_BUILD_TRANSLATOR_DEFAULT OFF)
else()
    set(HOT_BUILD_TRANSLATOR_DEFAULT ON)
endif()
option(HOT_BUILD_TRANSLATOR "Build the translator, the tools and the run-time backends (they need libclang)" ${HOT_BUILD_TRANSLATOR_DEFAULT})
set(HOT_HOST_HOTC "" CACHE FILEPATH "hotc that runs on the build host (cross builds)")
# The compile of a script module at run time (the clang files backend). An empty target or
# sysroot adds no flag. A MinGW target adds the libstdc++ headers of the sysroot.
set(HOT_SCRIPT_COMPILER "${CMAKE_CXX_COMPILER}" CACHE STRING "The compiler of the script modules at run time")
set(HOT_HOTC_TARGET "" CACHE STRING "The target triple of the script modules (--target)")
set(HOT_SCRIPT_SYSROOT "" CACHE PATH "The sysroot of the script modules (--sysroot)")

# Put the build output in bin/ and lib/. Ninja Multi-Config adds /<CONFIG> to each path.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

enable_testing()

# The Hot++ backends. The program chooses one at run time (make_default_backend, hot/backend.h).
#   HOT_ENABLE_CLANG_FILES: build the clang files backend into hot::backend. It translates in
#                           memory, runs an external clang++ and loads the .so with dlopen.
#   HOT_ENABLE_JIT:         build the JIT backend into hot::backend. It translates and compiles in
#                           memory with the Clang library and loads with LLVM ORC. It needs the
#                           LLVM and Clang CMake packages. AUTO: on when CMake finds the packages.
# The old option HOT_BACKEND picked one backend at build time. Map it once, then remove it from
# the cache, so a later -D value of the new options wins.
#   JIT:          HOT_ENABLE_CLANG_FILES=OFF and HOT_ENABLE_JIT=ON. Configure does not override a
#                 new option that is already in the cache.
#   CLANG_FILES:  nothing. It was the old default, and an old build folder always has it.
if(DEFINED HOT_BACKEND)
    if(HOT_BACKEND STREQUAL "JIT")
        if(DEFINED CACHE{HOT_ENABLE_JIT} OR DEFINED CACHE{HOT_ENABLE_CLANG_FILES})
            message(STATUS "HOT_BACKEND=JIT is ignored. HOT_ENABLE_CLANG_FILES or HOT_ENABLE_JIT is already set.")
        else()
            set(HOT_ENABLE_CLANG_FILES OFF CACHE BOOL "" FORCE)
            set(HOT_ENABLE_JIT ON CACHE STRING "" FORCE)
            message(WARNING "HOT_BACKEND is deprecated. The program now chooses the backend at run "
                    "time. Configure maps HOT_BACKEND=JIT to HOT_ENABLE_CLANG_FILES=OFF and "
                    "HOT_ENABLE_JIT=ON. Use these options.")
        endif()
    elseif(NOT HOT_BACKEND STREQUAL "CLANG_FILES")
        message(FATAL_ERROR "HOT_BACKEND must be CLANG_FILES or JIT. It is: ${HOT_BACKEND}")
    endif()
    unset(HOT_BACKEND CACHE)
    unset(HOT_BACKEND)
endif()

option(HOT_ENABLE_CLANG_FILES "Build the clang files backend (external clang++) into hot::backend" ON)
set(HOT_ENABLE_JIT AUTO CACHE STRING "Build the JIT backend into hot::backend: AUTO, ON or OFF")
set_property(CACHE HOT_ENABLE_JIT PROPERTY STRINGS AUTO ON OFF)

# All scratch files go under one root. HOT_KEEP_TEMP keeps them for debugging.
option(HOT_KEEP_TEMP "Keep the scratch files under tmp/ for debugging" OFF)
set(HOT_SCRATCH_DIR ${CMAKE_BINARY_DIR}/tmp)

# HOT_PORTABLE keeps build paths out of the binaries. The host finds the paths at run time
# (include/hot/paths.h). The tests need the build paths, so the two options exclude each other.
option(HOT_PORTABLE "No build paths in binaries" OFF)
if(HOT_PORTABLE AND HOT_BUILD_TESTS)
    message(FATAL_ERROR "HOT_PORTABLE and HOT_BUILD_TESTS are both ON. The tests need the build paths. Set HOT_BUILD_TESTS=OFF.")
endif()

# The default standard is C++17. A parent project can set CMAKE_CXX_STANDARD to 20 or 23.
# The string interpolation then uses std::format (see cmake/HotFormat.cmake).
if(NOT CMAKE_CXX_STANDARD)
    set(CMAKE_CXX_STANDARD 17)
    # The scope of a parent project does not see the variable. hot_add_scripts in that scope
    # reads this property (hot_effective_cxx_standard), so it agrees with the build of hotc.
    set_property(GLOBAL PROPERTY HOT_DEFAULT_CXX_STANDARD 17)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(MSVC)
    # /utf-8: the sources are UTF-8. /bigobj: the translator and the tests have large objects.
    # /permissive- and /Zc:__cplusplus: standard C++ rules and a correct value of __cplusplus.
    # /Zc:preprocessor: the standard preprocessor (the macros of the runtime headers need it).
    add_compile_options(/W4 /utf-8 /bigobj /permissive- /Zc:__cplusplus /Zc:preprocessor)
    # windows.h: no min and max macros, and only the core of the API.
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
    # /JMC (Just My Code) helps the debugger skip the library code.
    add_compile_options($<$<CONFIG:Debug>:/JMC>)
else()
    add_compile_options(-Wall -Wextra)
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
# Without LEXY_FORCE_CPP17, lexy asks for C++20 and so raises the standard of the translator.
if(CMAKE_CXX_STANDARD LESS 20)
    set(HOT_LEXY_FORCE_CPP17 ON)
else()
    set(HOT_LEXY_FORCE_CPP17 OFF)
endif()
CPMAddPackage(
    NAME lexy
    GITHUB_REPOSITORY foonathan/lexy
    GIT_TAG v2025.05.0
    VERSION 2025.05.0
    OPTIONS "LEXY_FORCE_CPP17 ${HOT_LEXY_FORCE_CPP17}" "LEXY_BUILD_EXAMPLES OFF" "LEXY_BUILD_TESTS OFF" "LEXY_BUILD_PACKAGE OFF" "LEXY_ENABLE_INSTALL OFF"
)

# nlohmann_json: the language server in src/ls uses it.
if(HOT_BUILD_LS AND HOT_BUILD_TRANSLATOR)
    CPMAddPackage(
        NAME nlohmann_json
        VERSION 3.12.0
        URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
        OPTIONS "JSON_BuildTests OFF" "JSON_Install OFF"
    )
endif()

# The script runtime and the language server use threads.
find_package(Threads REQUIRED)

# libclang and clang++ from the LLVM release for Windows. It sets HOT_CLANG_RESOURCE_DIR.
if(HOT_LLVM_DIR OR HOT_DOWNLOAD_LLVM)
    include(cmake/HotDownloadLlvm.cmake)
endif()

# The Clang resource directory. The translator, the backends and the tests use it.
if(HOT_CLANG_RESOURCE_DIR)
    # A value from the LLVM release, from a preset or from the command line.
    set(CLANG_RESOURCE_DIR "${HOT_CLANG_RESOURCE_DIR}")
elseif(NOT MSVC)
    # Only Clang knows the option. Another compiler (GCC, MinGW) gives an empty value.
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        execute_process(COMMAND ${CMAKE_CXX_COMPILER} -print-resource-dir
                        OUTPUT_VARIABLE CLANG_RESOURCE_DIR OUTPUT_STRIP_TRAILING_WHITESPACE)
    else()
        set(CLANG_RESOURCE_DIR "")
    endif()
else()
    # MSVC has no -print-resource-dir. The LLVM release (cmake/HotDownloadLlvm.cmake) sets
    # HOT_CLANG_RESOURCE_DIR. Without the release, set it to the resource directory of a Clang install.
    set(HOT_CLANG_RESOURCE_DIR "" CACHE PATH "The Clang resource directory (MSVC builds)")
    set(CLANG_RESOURCE_DIR "${HOT_CLANG_RESOURCE_DIR}")
endif()

# JitBackend (src/runtime): LLVM and Clang come from the system. CPM does not fetch them.
# HOT_JIT_ENABLED and HOT_CLANG_FILES_ENABLED hold the resolved choice.
set(HOT_CLANG_FILES_ENABLED ${HOT_ENABLE_CLANG_FILES})
string(TOUPPER "${HOT_ENABLE_JIT}" HOT_JIT_MODE)
if(NOT HOT_BUILD_TRANSLATOR)
    # Both backends need the translator (hotc_lib).
    set(HOT_CLANG_FILES_ENABLED OFF)
    set(HOT_JIT_MODE OFF)
endif()
if(NOT HOT_JIT_MODE MATCHES "^(AUTO|ON|OFF)$")
    message(FATAL_ERROR "HOT_ENABLE_JIT must be AUTO, ON or OFF. It is: ${HOT_ENABLE_JIT}")
endif()
set(HOT_JIT_ENABLED OFF)
if(NOT HOT_JIT_MODE STREQUAL "OFF")
    # LLVMConfig.cmake probes C headers (FindLibEdit), so the C language must be on.
    enable_language(C)
    find_package(LLVM CONFIG QUIET)
    if(LLVM_FOUND)
        find_package(Clang CONFIG QUIET)
    endif()
    if(LLVM_FOUND AND Clang_FOUND)
        set(HOT_JIT_ENABLED ON)
        message(STATUS "JIT backend: on (LLVM ${LLVM_PACKAGE_VERSION}, Clang ${Clang_VERSION})")
    elseif(HOT_JIT_MODE STREQUAL "ON")
        message(FATAL_ERROR "HOT_ENABLE_JIT=ON needs the LLVM and Clang CMake packages. CMake does not "
                "find them. Install the LLVM and Clang development packages, or add their folders to "
                "CMAKE_PREFIX_PATH, or set HOT_ENABLE_JIT=AUTO or OFF.")
    else()
        message(STATUS "JIT backend: off (HOT_ENABLE_JIT=AUTO, no LLVM and Clang CMake packages)")
    endif()
elseif(NOT HOT_BUILD_TRANSLATOR)
    message(STATUS "JIT backend: off (HOT_BUILD_TRANSLATOR=OFF)")
else()
    message(STATUS "JIT backend: off (HOT_ENABLE_JIT=OFF)")
endif()
if(HOT_BUILD_TRANSLATOR)
    message(STATUS "Clang files backend: ${HOT_CLANG_FILES_ENABLED}")
else()
    message(STATUS "Clang files backend: off (HOT_BUILD_TRANSLATOR=OFF)")
endif()
if(NOT HOT_CLANG_FILES_ENABLED AND NOT HOT_JIT_ENABLED)
    message(STATUS "No run-time backend: hot::backend is not built. Only AOT and PRECOMPILED hosts work.")
    if(HOT_BUILD_TESTS AND NOT HOT_BUILD_TRANSLATOR)
        message(STATUS "The tests that need a run-time backend are not built (HOT_BUILD_TRANSLATOR=OFF).")
    elseif(HOT_BUILD_TESTS)
        message(FATAL_ERROR "The tests need a run-time backend. Set HOT_ENABLE_CLANG_FILES=ON or "
                "HOT_ENABLE_JIT=ON, or set HOT_BUILD_TESTS=OFF.")
    endif()
endif()

# The runtime checks of the scripts: the null test of a pointer and a handle, and the bounds test
# of an array (include/hot/runtime/errors.h). OFF removes them. The runtime library defines
# HOT_RUNTIME_CHECKS=0 for the host and for the installed SDK (src/runtime/CMakeLists.txt).
option(HOT_RUNTIME_CHECKS "Check null dereference and array bounds in scripts" ON)

# The script traceback (include/hot/runtime/trace.h). ON makes each script function record a frame
# and each script exception keep the call stack of its throw point. The cost is a few stores in
# each statement. The runtime library defines HOT_TRACEBACK=1 for the host
# (src/runtime/CMakeLists.txt).
option(HOT_TRACEBACK "Record the script call stack for the error report" OFF)

# The thread safety of the runtime (include/hot/config.h). ON: any thread can create and destroy
# script objects, because the list of live objects has a small lock. OFF: no lock, and only one
# thread creates and destroys script objects. The runtime library defines HOT_THREAD_SAFE=0 for the
# host and for the installed SDK (src/runtime/CMakeLists.txt). The host and the runtime
# must use the same value. The constructor of the Loader checks this.
option(HOT_THREAD_SAFE "Allow any thread to create and destroy script objects" ON)

# The view type of "T[]" (include/hot/config.h): hot::span<T> ("hot") or std::span<T> ("std").
# One value for the whole build. "std" needs C++20.
set(HOT_VIEW_TYPE "hot" CACHE STRING "View type of T[]: hot (hot::span) or std (std::span, C++20)")
set_property(CACHE HOT_VIEW_TYPE PROPERTY STRINGS hot std)
if(NOT HOT_VIEW_TYPE STREQUAL "hot" AND NOT HOT_VIEW_TYPE STREQUAL "std")
    message(FATAL_ERROR "HOT_VIEW_TYPE is '${HOT_VIEW_TYPE}'. The value must be 'hot' or 'std'.")
endif()
if(HOT_VIEW_TYPE STREQUAL "std" AND CMAKE_CXX_STANDARD LESS 20)
    message(FATAL_ERROR "HOT_VIEW_TYPE=std needs C++20 (std::span). CMAKE_CXX_STANDARD is ${CMAKE_CXX_STANDARD}. "
            "Set CMAKE_CXX_STANDARD to 20 or 23, or set HOT_VIEW_TYPE=hot.")
endif()
message(STATUS "The view type of T[] is: ${HOT_VIEW_TYPE}")

# The compiler flags for the scripts. The backends use them.
# A list value uses "|" between the items.
# MSVC: the flags of cl (/EHsc, /DWIN32) are not flags of the script compiler (clang++).
if(MSVC)
    set(HOT_CXXFLAGS_LIST "")
else()
    separate_arguments(HOT_CXXFLAGS_LIST NATIVE_COMMAND "${CMAKE_CXX_FLAGS}")
endif()
if(NOT HOT_RUNTIME_CHECKS)
    list(APPEND HOT_CXXFLAGS_LIST -DHOT_RUNTIME_CHECKS=0)
endif()
if(HOT_TRACEBACK)
    list(APPEND HOT_CXXFLAGS_LIST -DHOT_TRACEBACK=1)
endif()
if(NOT HOT_THREAD_SAFE)
    list(APPEND HOT_CXXFLAGS_LIST -DHOT_THREAD_SAFE=0)
endif()
if(HOT_VIEW_TYPE STREQUAL "std")
    list(APPEND HOT_CXXFLAGS_LIST -DHOT_VIEW_STD=1)
endif()
list(JOIN HOT_CXXFLAGS_LIST "|" HOT_CXXFLAGS_JOINED)

# The headers that the precompiled and AOT scripts depend on (the engine headers are in tests/engine). The tests use them.
if(HOT_BUILD_TESTS)
    file(GLOB HOT_RUNTIME_HEADERS ${PROJECT_SOURCE_DIR}/include/hot/runtime/*.h)
    set(HOT_HEADERS ${PROJECT_SOURCE_DIR}/tests/engine/Actor.h ${PROJECT_SOURCE_DIR}/tests/engine/EngineMath.h
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

# GoogleTest targets. They need the pre_* targets and hotc from above.
if(HOT_BUILD_TESTS)
    add_subdirectory(tests)
    # The benchmark uses the test engine and the test scripts.
    if(HOT_BUILD_TRANSLATOR)
        add_subdirectory(tools/bench)
    endif()

    # The test of the installed SDK: install, then build samples/aot against the prefix.
    # It needs the whole build, because "cmake --install" installs all targets.
    if(HOT_INSTALL AND HOT_BUILD_TRANSLATOR)
        add_test(NAME sdk_install_sample
            COMMAND ${CMAKE_COMMAND}
                -DBUILD_DIR=${CMAKE_BINARY_DIR} -DSAMPLE_DIR=${PROJECT_SOURCE_DIR}/samples/aot
                -DWORK_DIR=${CMAKE_BINARY_DIR}/sdk_install
                -DCXX_COMPILER=${CMAKE_CXX_COMPILER} "-DCXX_FLAGS=${CMAKE_CXX_FLAGS}"
                -DGENERATOR=${CMAKE_GENERATOR} -DCONFIG=$<CONFIG>
                -P ${PROJECT_SOURCE_DIR}/cmake/SdkInstallTest.cmake)
        set_tests_properties(sdk_install_sample PROPERTIES LABELS sdk TIMEOUT 600)
    endif()
endif()

# The VS Code extension. The folder has the node tests (HOT_BUILD_TESTS) and the install rules.
if(HOT_BUILD_VSCODE)
    add_subdirectory(editors/vscode)
endif()

# The tree-sitter grammar. The folder has one test. It needs node and the tree-sitter tool.
add_subdirectory(editors/tree-sitter-hot)

if(HOT_BUILD_SAMPLES)
    add_subdirectory(samples)
endif()

# ------------------------------------------------------------ Install (the relocatable SDK)
# "cmake --install" makes a prefix. A consumer uses find_package(Hot) and hot_add_host.
# HOT_PORTABLE=ON and HOT_BUILD_TESTS=OFF make an SDK without build paths in the binaries.
if(HOT_INSTALL)
    include(CMakePackageConfigHelpers)
    set(HOT_CMAKE_INSTALL_DIR ${CMAKE_INSTALL_LIBDIR}/cmake/Hot)

    # The names in the namespace hot:: (hot::runtime, hot::aot, ...).
    set_target_properties(hot_runtime PROPERTIES EXPORT_NAME runtime)
    set_target_properties(hot_runtime_core PROPERTIES EXPORT_NAME runtime_core)
    set_target_properties(hot_aot PROPERTIES EXPORT_NAME aot)
    set(HOT_INSTALL_TARGETS hot_runtime hot_runtime_core hot_aot)
    if(HOT_BUILD_TRANSLATOR)
        set_target_properties(hotc_lib PROPERTIES EXPORT_NAME hotc_lib)
        set_target_properties(hotc PROPERTIES EXPORT_NAME hotc)
        list(APPEND HOT_INSTALL_TARGETS hotc_lib hotc)
        if(HOT_BUILD_LS)
            set_target_properties(hot_ls PROPERTIES EXPORT_NAME hot_ls)
            list(APPEND HOT_INSTALL_TARGETS hot_ls)
        endif()
    endif()
    if(TARGET hot_backend)
        set_target_properties(hot_backend PROPERTIES EXPORT_NAME backend)
        list(APPEND HOT_INSTALL_TARGETS hot_backend)
    endif()
    if(HOT_CLANG_FILES_ENABLED)
        set_target_properties(hot_clang_backend PROPERTIES EXPORT_NAME clang_backend)
        list(APPEND HOT_INSTALL_TARGETS hot_clang_backend)
    endif()
    if(TARGET hot_jit_backend)
        set_target_properties(hot_jit_backend PROPERTIES EXPORT_NAME jit_backend)
        list(APPEND HOT_INSTALL_TARGETS hot_jit_backend)
    endif()

    install(TARGETS ${HOT_INSTALL_TARGETS} EXPORT HotTargets
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
    install(DIRECTORY include/hot DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    install(EXPORT HotTargets NAMESPACE hot:: DESTINATION ${HOT_CMAKE_INSTALL_DIR})

    # The values of this build that the package config sets for a consumer.
    hot_effective_cxx_standard(HOT_SDK_CXX_STANDARD hot_runtime_core)
    get_filename_component(HOT_CLANG_C_LIBRARY_DIR "${HOT_LIBCLANG_FILE}" DIRECTORY)
    set(HOT_CLANG_C_LIBRARY "${HOT_LIBCLANG_FILE}")
    # The folder of the Clang package of this build (empty if the build used the fallback).
    set(HOT_CLANG_CMAKE_DIR "")
    if(HOT_LIBCLANG_FROM_PACKAGE)
        set(HOT_CLANG_CMAKE_DIR "${Clang_DIR}")
    endif()
    # The script compiler goes to a consumer only in a cross build (HOT_HOTC_TARGET is set).
    # In a native build it stays empty, so the consumer uses its own CMAKE_CXX_COMPILER.
    set(HOT_SDK_SCRIPT_COMPILER "")
    if(HOT_HOTC_TARGET)
        set(HOT_SDK_SCRIPT_COMPILER "${HOT_SCRIPT_COMPILER}")
    endif()
    configure_package_config_file(cmake/HotConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/HotConfig.cmake
        INSTALL_DESTINATION ${HOT_CMAKE_INSTALL_DIR})
    write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/HotConfigVersion.cmake
        VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/HotConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/HotConfigVersion.cmake
        cmake/HotScripts.cmake
        cmake/HotFormat.cmake
        cmake/CPM.cmake
        DESTINATION ${HOT_CMAKE_INSTALL_DIR})
    install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/hot)
endif()
