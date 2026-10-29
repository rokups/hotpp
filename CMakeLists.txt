cmake_minimum_required(VERSION 3.20)
project(spp CXX)

enable_testing()

# The S++ backend, chosen at build time (design section 3.3).
#   CLANG_FILES: translate in memory, run an external clang++, dlopen the .so.
#   JIT:         translate and compile in memory with the Clang library, load with LLVM ORC.
set(SPP_BACKEND CLANG_FILES CACHE STRING "S++ backend: CLANG_FILES or JIT")
set_property(CACHE SPP_BACKEND PROPERTY STRINGS CLANG_FILES JIT)
if(NOT SPP_BACKEND STREQUAL "JIT" AND NOT SPP_BACKEND STREQUAL "CLANG_FILES")
    message(FATAL_ERROR "SPP_BACKEND must be CLANG_FILES or JIT. It is: ${SPP_BACKEND}")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
add_compile_options(-Wall -Wextra)

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
include(GoogleTest)
# lexy: header-only parser library. Only src/translator/parser/ includes its headers.
# Use only foonathan::lexy::core. Do not link foonathan::lexy (it builds lexy_file).
CPMAddPackage(
    NAME lexy
    GITHUB_REPOSITORY foonathan/lexy
    GIT_TAG v2025.05.0
    VERSION 2025.05.0
    OPTIONS "LEXY_BUILD_EXAMPLES OFF" "LEXY_BUILD_TESTS OFF" "LEXY_BUILD_PACKAGE OFF" "LEXY_ENABLE_INSTALL OFF"
)

# Script runtime. It has two parts.
#   spp_runtime_core: the globals and the registry that generated code needs. A shipping
#     build links only this part (design M7). It has no loader, no migration, no dlopen.
#   spp_runtime: the core plus the state tracking, the migration and the generation loader.
find_package(Threads REQUIRED)
add_library(spp_runtime_core STATIC src/runtime/threads.cpp src/runtime/registry.cpp)
target_include_directories(spp_runtime_core PUBLIC ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/example/engine)
add_library(spp_runtime STATIC
    src/runtime/state.cpp src/runtime/migration.cpp src/runtime/loader.cpp src/runtime/module.cpp)
target_link_libraries(spp_runtime PUBLIC spp_runtime_core ${CMAKE_DL_LIBS} Threads::Threads)

# Shipping runtime (design M7): the core, the shipped registry and no-op object tracking.
# Do not link it together with spp_runtime.
add_library(spp_ship STATIC src/runtime/shipped.cpp)
target_link_libraries(spp_ship PUBLIC spp_runtime_core Threads::Threads)

# Engine harness. ENABLE_EXPORTS adds -rdynamic, so a generation module
# resolves the engine symbols and the runtime symbols from the executable.
# The file watcher and the command channel are in the library engine_harness. The tests use it.
add_library(engine_harness STATIC example/engine/FileWatcher.cpp)
target_include_directories(engine_harness PUBLIC ${CMAKE_SOURCE_DIR}/example/engine)
add_executable(minengine example/engine/main.cpp example/engine/Harness.cpp
    example/engine/Actor.cpp example/engine/Util.cpp)
target_link_libraries(minengine PRIVATE spp_backend engine_harness)
set_target_properties(minengine PROPERTIES ENABLE_EXPORTS ON)

# ------------------------------------------------------------ Step F1
# The translator library sppc_lib. It needs libclang (C API) from the system.
# The tool sppc (below) uses it.
find_library(CLANG_C_LIBRARY NAMES clang REQUIRED)
find_path(CLANG_C_INCLUDE_DIR clang-c/Index.h REQUIRED)
execute_process(COMMAND ${CMAKE_CXX_COMPILER} -print-resource-dir
                OUTPUT_VARIABLE CLANG_RESOURCE_DIR OUTPUT_STRIP_TRAILING_WHITESPACE)
add_library(sppc_lib STATIC src/translator/sppc_lib.cpp src/translator/emit.cpp src/translator/lower.cpp src/translator/types.cpp src/translator/check.cpp src/translator/diag_rewrite.cpp
    src/translator/parser/script_grammar.cpp
    src/translator/parser/expression_grammar.cpp src/translator/parser/statement_grammar.cpp)
target_include_directories(sppc_lib PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_include_directories(sppc_lib PRIVATE ${CLANG_C_INCLUDE_DIR})
target_link_libraries(sppc_lib PUBLIC ${CLANG_C_LIBRARY})
target_link_libraries(sppc_lib PRIVATE foonathan::lexy::core)
target_compile_definitions(sppc_lib PRIVATE SPP_RESOURCE_DIR="${CLANG_RESOURCE_DIR}")

# ------------------------------------------------------------ Backends
# ClangFilesBackend: sppc_lib in memory, then an external compiler.
add_library(spp_clang_backend STATIC src/runtime/clang_files_backend.cpp)
target_link_libraries(spp_clang_backend PUBLIC spp_runtime sppc_lib)

# sppc is the S++ tool. It translates a .spp file (--emit-cpp) or compiles it ahead
# of time into a .so. The output loads in every build.
add_executable(sppc src/tools/sppc.cpp)
target_link_libraries(sppc PRIVATE spp_clang_backend)

# JitBackend: sppc_lib in memory, the Clang library compiles in memory, LLVM ORC loads.
# LLVM and Clang come from the system. CPM does not fetch them.
if(SPP_BACKEND STREQUAL "JIT")
    # LLVMConfig.cmake probes C headers (FindLibEdit), so the C language must be on.
    enable_language(C)
    find_package(LLVM REQUIRED CONFIG)
    find_package(Clang REQUIRED CONFIG)
    message(STATUS "JIT backend: LLVM ${LLVM_PACKAGE_VERSION}, Clang ${Clang_VERSION}")
    add_library(spp_jit_backend STATIC src/runtime/jit_backend.cpp)
    target_include_directories(spp_jit_backend PUBLIC ${CMAKE_SOURCE_DIR}/include)
    target_include_directories(spp_jit_backend SYSTEM PUBLIC ${LLVM_INCLUDE_DIRS} ${CLANG_INCLUDE_DIRS})
    target_compile_definitions(spp_jit_backend PRIVATE ${LLVM_DEFINITIONS})
    target_link_libraries(spp_jit_backend PUBLIC spp_runtime sppc_lib clang-cpp LLVM)
endif()

# make_default_backend() returns the backend that SPP_BACKEND selected.
# A list value uses "|" between the items.
separate_arguments(SPP_CXXFLAGS_LIST NATIVE_COMMAND "${CMAKE_CXX_FLAGS}")
list(JOIN SPP_CXXFLAGS_LIST "|" SPP_CXXFLAGS_JOINED)
add_library(spp_backend STATIC src/runtime/default_backend.cpp)
target_link_libraries(spp_backend PUBLIC spp_runtime)
if(SPP_BACKEND STREQUAL "CLANG_FILES")
    target_link_libraries(spp_backend PUBLIC spp_clang_backend)
    target_compile_definitions(spp_backend PRIVATE
        SPP_BACKEND_CLANG_FILES=1
        SPP_DEFAULT_CXX="${CMAKE_CXX_COMPILER}"
        SPP_DEFAULT_CXXFLAGS="${SPP_CXXFLAGS_JOINED}"
        SPP_DEFAULT_INCLUDES="${CMAKE_SOURCE_DIR}/include|${CMAKE_SOURCE_DIR}/example/engine"
        SPP_DEFAULT_RESOURCE_DIR="${CLANG_RESOURCE_DIR}"
        SPP_DEFAULT_WORK_DIR="${CMAKE_BINARY_DIR}/spp_work")
elseif(SPP_BACKEND STREQUAL "JIT")
    target_link_libraries(spp_backend PUBLIC spp_jit_backend)
    target_compile_definitions(spp_backend PRIVATE
        SPP_BACKEND_JIT=1
        SPP_DEFAULT_CXX="${CMAKE_CXX_COMPILER}"
        SPP_DEFAULT_CXXFLAGS="${SPP_CXXFLAGS_JOINED}"
        SPP_DEFAULT_INCLUDES="${CMAKE_SOURCE_DIR}/include|${CMAKE_SOURCE_DIR}/example/engine"
        SPP_DEFAULT_RESOURCE_DIR="${CLANG_RESOURCE_DIR}")
endif()

# ------------------------------------------------------------ Language server (S14b)
# spp-ls is a stdio LSP proxy. It translates a .spp document in memory and gives the generated
# C++ to a stock clangd child process. It maps the positions with the source map. The library
# spp_ls_lib holds the server, so the tests run it in the test process over pipes.
CPMAddPackage(
    NAME nlohmann_json
    VERSION 3.12.0
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
    OPTIONS "JSON_BuildTests OFF" "JSON_Install OFF"
)
add_library(spp_ls_lib STATIC src/ls/jsonrpc.cpp src/ls/clangd_process.cpp src/ls/server.cpp
    src/ls/semantic.cpp src/ls/symbol_index.cpp src/ls/hosts.cpp)
target_include_directories(spp_ls_lib PUBLIC ${CMAKE_SOURCE_DIR}/src/ls PRIVATE ${CMAKE_SOURCE_DIR}/src/translator)
target_link_libraries(spp_ls_lib PUBLIC spp_clang_backend nlohmann_json::nlohmann_json Threads::Threads)
add_executable(spp_ls src/ls/main.cpp)
target_link_libraries(spp_ls PRIVATE spp_ls_lib)
set_target_properties(spp_ls PROPERTIES OUTPUT_NAME spp-ls)

include(cmake/SppScripts.cmake)

# ------------------------------------------------------------ Dear ImGui example
# imgui_app draws a Dear ImGui user interface from a script. It needs SDL3.
# Without SDL3 the target does not exist and the rest of the build is unchanged.
find_package(SDL3 QUIET)
if(SDL3_FOUND AND NOT CMAKE_VERSION VERSION_LESS 3.24)
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

    # The host builds ClangFilesOptions itself. It does not use make_default_backend().
    add_executable(imgui_app example/imgui/main.cpp example/imgui/UiApp.cpp)
    target_include_directories(imgui_app PRIVATE ${CMAKE_SOURCE_DIR}/example/imgui)
    # Whole archive: a script can call any ImGui function. The linker must keep all objects.
    target_link_libraries(imgui_app PRIVATE spp_runtime spp_clang_backend "$<LINK_LIBRARY:WHOLE_ARCHIVE,imgui_sdl3>")
    set_target_properties(imgui_app PROPERTIES ENABLE_EXPORTS ON)
    target_compile_definitions(imgui_app PRIVATE
        IMGUI_APP_CXX="${CMAKE_CXX_COMPILER}"
        IMGUI_APP_CXXFLAGS="${SPP_CXXFLAGS_JOINED}"
        IMGUI_APP_INCLUDES="${CMAKE_SOURCE_DIR}/include|${CMAKE_SOURCE_DIR}/example/engine|${CMAKE_SOURCE_DIR}/example/imgui|${imgui_SOURCE_DIR}"
        IMGUI_APP_RESOURCE_DIR="${CLANG_RESOURCE_DIR}"
        IMGUI_APP_WORK_DIR="${CMAKE_BINARY_DIR}/imgui_work"
        IMGUI_APP_DEFAULT_SCRIPT="${CMAKE_SOURCE_DIR}/example/imgui/demo.spp")
    # The language server reads this file. The engine dir is in IMGUI_APP_INCLUDES.
    spp_script_host(NAME imgui_app TARGET imgui_app
        SCRIPT_DIRS ${CMAKE_SOURCE_DIR}/example/imgui
        INCLUDES ${CMAKE_SOURCE_DIR}/example/engine)
else()
    message(STATUS "SDL3 or CMake 3.24 not found: the target imgui_app is not built")
endif()

# ------------------------------------------------------------ Precompiled scripts
file(GLOB SPP_RUNTIME_HEADERS ${CMAKE_SOURCE_DIR}/include/spp/runtime/*.h)
set(SPP_HEADERS ${CMAKE_SOURCE_DIR}/example/engine/Actor.h ${CMAKE_SOURCE_DIR}/example/engine/Math.h
                ${CMAKE_SOURCE_DIR}/include/spp/runtime.h ${CMAKE_SOURCE_DIR}/include/spp/module.h
                ${SPP_RUNTIME_HEADERS})
spp_add_scripts(TARGET pre_gen1 GEN 1 SCRIPTS example/scripts/player_v1.spp
                INCLUDES ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/example/engine
                FLAGS ${SPP_CXXFLAGS_LIST} DEPENDS ${SPP_HEADERS})
spp_add_scripts(TARGET pre_gen2 GEN 2 SCRIPTS example/scripts/player_v2.spp AFTER pre_gen1
                INCLUDES ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/example/engine
                FLAGS ${SPP_CXXFLAGS_LIST} DEPENDS ${SPP_HEADERS})
spp_add_scripts(TARGET pre_native1 GEN 1 SCRIPTS example/scripts/native_std_v1.spp AFTER pre_gen2
                INCLUDES ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/example/engine
                FLAGS ${SPP_CXXFLAGS_LIST} DEPENDS ${SPP_HEADERS} ${CMAKE_SOURCE_DIR}/example/engine/Util.h)
spp_add_scripts(TARGET pre_native2 GEN 2 SCRIPTS example/scripts/native_std_v2.spp AFTER pre_native1
                INCLUDES ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/example/engine
                FLAGS ${SPP_CXXFLAGS_LIST} DEPENDS ${SPP_HEADERS} ${CMAKE_SOURCE_DIR}/example/engine/Util.h)
spp_add_scripts(TARGET pre_handle1 GEN 1 SCRIPTS example/scripts/handle_v1.spp AFTER pre_native2
                INCLUDES ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/example/engine
                FLAGS ${SPP_CXXFLAGS_LIST} DEPENDS ${SPP_HEADERS} ${CMAKE_SOURCE_DIR}/example/engine/Weapon.h)
spp_add_scripts(TARGET pre_handle2 GEN 2 SCRIPTS example/scripts/handle_v2.spp AFTER pre_handle1
                INCLUDES ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/example/engine
                FLAGS ${SPP_CXXFLAGS_LIST} DEPENDS ${SPP_HEADERS} ${CMAKE_SOURCE_DIR}/example/engine/Weapon.h)
spp_add_scripts(TARGET pre_handle3 GEN 3 SCRIPTS example/scripts/handle_v1.spp AFTER pre_handle2
                INCLUDES ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/example/engine
                FLAGS ${SPP_CXXFLAGS_LIST} DEPENDS ${SPP_HEADERS} ${CMAKE_SOURCE_DIR}/example/engine/Weapon.h)

# ------------------------------------------------------------ Shipping mode (M7)
# ship_demo has the C++ of a script in the binary. It links spp_ship and not spp_runtime, so it has
# no loader, no migration, no dlopen and no Clang. Actor.cpp and Util.cpp are the engine.
spp_add_shipped_scripts(TARGET ship_demo_scripts SCRIPTS example/scripts/ship_demo.spp
                        INCLUDES ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/example/engine
                        DEPENDS ${SPP_HEADERS})
add_executable(ship_demo example/ship/main.cpp example/engine/Actor.cpp example/engine/Util.cpp)
target_include_directories(ship_demo PRIVATE ${CMAKE_SOURCE_DIR}/example/ship)
target_link_libraries(ship_demo PRIVATE ship_demo_scripts)

# GoogleTest targets. They need the pre_* targets and sppc from above.
add_subdirectory(tests)
add_subdirectory(editors/vscode)

# bench_backends times the compile and the load of the example scripts.
add_executable(bench_backends example/bench_backends.cpp example/engine/Actor.cpp example/engine/Util.cpp)
set_target_properties(bench_backends PROPERTIES ENABLE_EXPORTS ON)
target_link_libraries(bench_backends PRIVATE spp_backend)
if(SPP_BACKEND STREQUAL "JIT")
    target_link_libraries(bench_backends PRIVATE spp_jit_backend)
    target_compile_definitions(bench_backends PRIVATE BENCH_JIT=1)
else()
    target_link_libraries(bench_backends PRIVATE spp_clang_backend)
    target_compile_definitions(bench_backends PRIVATE BENCH_CLANG_FILES=1)
endif()
target_compile_definitions(bench_backends PRIVATE
    BENCH_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    BENCH_CXX="${CMAKE_CXX_COMPILER}"
    BENCH_INCLUDES="${CMAKE_SOURCE_DIR}/include|${CMAKE_SOURCE_DIR}/example/engine"
    BENCH_RESOURCE_DIR="${CLANG_RESOURCE_DIR}"
    BENCH_WORK_DIR="${CMAKE_BINARY_DIR}/spp_work")
