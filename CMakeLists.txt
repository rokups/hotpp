cmake_minimum_required(VERSION 3.20)
project(spp CXX)

enable_testing()

# The script backend, chosen at build time (design section 3.3).
#   CLANG_FILES: translate in memory, run an external clang++, dlopen the .so.
#   JIT:         translate and compile in memory with the Clang library, load with LLVM ORC.
set(SPP_BACKEND CLANG_FILES CACHE STRING "Script backend: CLANG_FILES or JIT")
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
set(CPM_USE_LOCAL_PACKAGES ON)
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

# Script runtime: a small static library for the executable.
add_library(spp_runtime STATIC
    src/runtime/threads.cpp src/runtime/state.cpp src/runtime/registry.cpp
    src/runtime/migration.cpp src/runtime/loader.cpp src/runtime/module.cpp)
target_include_directories(spp_runtime PUBLIC ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/example/engine)
find_package(Threads REQUIRED)
target_link_libraries(spp_runtime PUBLIC ${CMAKE_DL_LIBS} Threads::Threads)

# Engine harness. ENABLE_EXPORTS adds -rdynamic, so a generation module
# resolves the engine symbols and the runtime symbols from the executable.
add_executable(minengine example/engine/main.cpp example/engine/Actor.cpp example/engine/Util.cpp)
target_link_libraries(minengine PRIVATE spp_runtime)
set_target_properties(minengine PROPERTIES ENABLE_EXPORTS ON)

# Step R0: generation 1 of the Player class.
add_library(gen1 SHARED example/poc/r0/gen1.cpp)
target_include_directories(gen1 PRIVATE ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/example/engine)
set_target_properties(gen1 PROPERTIES PREFIX "")

# Step R1: generation 2 (migration) and generation 4 (rejected: other shell).
# gen3_bad.cpp does not compile on purpose. It is not a target.
# See example/poc/r1/make_bad.sh.
foreach(g gen2:example/poc/r1/gen2.cpp gen4_base:example/poc/r1/gen4_base.cpp)
    string(REPLACE ":" ";" parts ${g})
    list(GET parts 0 gname)
    list(GET parts 1 gsrc)
    add_library(${gname} SHARED ${gsrc})
    target_include_directories(${gname} PRIVATE ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/example/engine)
    set_target_properties(${gname} PROPERTIES PREFIX "")
endforeach()

# GoogleTest cases R0 and R1. The file gen3_missing.so does not exist on purpose.
foreach(r r0 r1)
    add_executable(${r}_tests tests/${r}_tests.cpp example/engine/Actor.cpp example/engine/Util.cpp)
    target_link_libraries(${r}_tests PRIVATE spp_runtime GTest::gtest_main)
    set_target_properties(${r}_tests PROPERTIES ENABLE_EXPORTS ON)
    target_include_directories(${r}_tests PRIVATE tests)
    add_dependencies(${r}_tests gen1 gen2 gen4_base)
    target_compile_definitions(${r}_tests PRIVATE
        GEN1_SO="$<TARGET_FILE:gen1>"
        GEN2_SO="$<TARGET_FILE:gen2>"
        GEN4_BASE_SO="$<TARGET_FILE:gen4_base>"
        GEN3_MISSING_SO="${CMAKE_BINARY_DIR}/gen3_missing.so")
    gtest_discover_tests(${r}_tests)
endforeach()

# ------------------------------------------------------------ Step F1
# The translator library spc_lib and the tool spc. They need libclang (C API)
# from the system.
find_library(CLANG_C_LIBRARY NAMES clang REQUIRED)
find_path(CLANG_C_INCLUDE_DIR clang-c/Index.h REQUIRED)
execute_process(COMMAND ${CMAKE_CXX_COMPILER} -print-resource-dir
                OUTPUT_VARIABLE CLANG_RESOURCE_DIR OUTPUT_STRIP_TRAILING_WHITESPACE)
add_library(spc_lib STATIC src/translator/spc_lib.cpp src/translator/emit.cpp src/translator/lower.cpp src/translator/types.cpp
    src/translator/parser/script_grammar.cpp
    src/translator/parser/expression_grammar.cpp src/translator/parser/statement_grammar.cpp)
target_include_directories(spc_lib PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_include_directories(spc_lib PRIVATE ${CLANG_C_INCLUDE_DIR})
target_link_libraries(spc_lib PUBLIC ${CLANG_C_LIBRARY})
target_link_libraries(spc_lib PRIVATE foonathan::lexy::core)
target_compile_definitions(spc_lib PRIVATE SPC_RESOURCE_DIR="${CLANG_RESOURCE_DIR}")
add_executable(spc src/translator/spc.cpp)
target_link_libraries(spc PRIVATE spc_lib)

# ------------------------------------------------------------ Backends
# ClangFilesBackend: spc_lib in memory, then an external compiler.
add_library(spp_clang_backend STATIC src/runtime/clang_files_backend.cpp)
target_link_libraries(spp_clang_backend PUBLIC spp_runtime spc_lib)

# sppc compiles scripts ahead of time. Its output loads in every build.
add_executable(sppc src/tools/sppc.cpp)
target_link_libraries(sppc PRIVATE spp_clang_backend)

# JitBackend: spc_lib in memory, the Clang library compiles in memory, LLVM ORC loads.
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
    target_link_libraries(spp_jit_backend PUBLIC spp_runtime spc_lib clang-cpp LLVM)
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
        IMGUI_APP_DEFAULT_SCRIPT="${CMAKE_SOURCE_DIR}/example/imgui/demo.script")
else()
    message(STATUS "SDL3 or CMake 3.24 not found: the target imgui_app is not built")
endif()

# ------------------------------------------------------------ Precompiled scripts
include(cmake/SppScripts.cmake)
file(GLOB SPP_RUNTIME_HEADERS ${CMAKE_SOURCE_DIR}/include/spp/runtime/*.h)
set(SPP_HEADERS ${CMAKE_SOURCE_DIR}/example/engine/Actor.h ${CMAKE_SOURCE_DIR}/example/engine/Math.h
                ${CMAKE_SOURCE_DIR}/include/spp/script_runtime.h ${CMAKE_SOURCE_DIR}/include/spp/module.h
                ${SPP_RUNTIME_HEADERS})
spp_add_scripts(TARGET pre_gen1 GEN 1 SCRIPTS example/scripts/player_v1.script
                INCLUDES ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/example/engine
                FLAGS ${SPP_CXXFLAGS_LIST} DEPENDS ${SPP_HEADERS})
spp_add_scripts(TARGET pre_gen2 GEN 2 SCRIPTS example/scripts/player_v2.script AFTER pre_gen1
                INCLUDES ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/example/engine
                FLAGS ${SPP_CXXFLAGS_LIST} DEPENDS ${SPP_HEADERS})
spp_add_scripts(TARGET pre_native1 GEN 1 SCRIPTS example/scripts/native_std_v1.script AFTER pre_gen2
                INCLUDES ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/example/engine
                FLAGS ${SPP_CXXFLAGS_LIST} DEPENDS ${SPP_HEADERS} ${CMAKE_SOURCE_DIR}/example/engine/Util.h)
spp_add_scripts(TARGET pre_native2 GEN 2 SCRIPTS example/scripts/native_std_v2.script AFTER pre_native1
                INCLUDES ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/example/engine
                FLAGS ${SPP_CXXFLAGS_LIST} DEPENDS ${SPP_HEADERS} ${CMAKE_SOURCE_DIR}/example/engine/Util.h)
spp_add_scripts(TARGET pre_handle1 GEN 1 SCRIPTS example/scripts/handle_v1.script AFTER pre_native2
                INCLUDES ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/example/engine
                FLAGS ${SPP_CXXFLAGS_LIST} DEPENDS ${SPP_HEADERS} ${CMAKE_SOURCE_DIR}/example/engine/Weapon.h)
spp_add_scripts(TARGET pre_handle2 GEN 2 SCRIPTS example/scripts/handle_v2.script AFTER pre_handle1
                INCLUDES ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/example/engine
                FLAGS ${SPP_CXXFLAGS_LIST} DEPENDS ${SPP_HEADERS} ${CMAKE_SOURCE_DIR}/example/engine/Weapon.h)
spp_add_scripts(TARGET pre_handle3 GEN 3 SCRIPTS example/scripts/handle_v1.script AFTER pre_handle2
                INCLUDES ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/example/engine
                FLAGS ${SPP_CXXFLAGS_LIST} DEPENDS ${SPP_HEADERS} ${CMAKE_SOURCE_DIR}/example/engine/Weapon.h)

# GoogleTest cases: BackendTests (the selected backend) and PrecompiledTests (sppc).
add_executable(backend_tests tests/backend_tests.cpp example/engine/Actor.cpp example/engine/Object.cpp example/engine/Util.cpp)
target_include_directories(backend_tests PRIVATE tests)
target_link_libraries(backend_tests PRIVATE spp_backend GTest::gtest_main)
set_target_properties(backend_tests PROPERTIES ENABLE_EXPORTS ON)
add_dependencies(backend_tests pre_gen1 pre_gen2 pre_native1 pre_native2 pre_handle1 pre_handle2 pre_handle3 sppc)
target_compile_definitions(backend_tests PRIVATE
    SPP_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    SPP_BINARY_DIR="${CMAKE_BINARY_DIR}"
    SPP_BACKEND_NAME="${SPP_BACKEND}"
    SPPC_PATH="$<TARGET_FILE:sppc>"
    SPP_CXX="${CMAKE_CXX_COMPILER}"
    PRE_V1_SO="${CMAKE_BINARY_DIR}/spp_scripts/player_v1.g1.so"
    PRE_V2_SO="${CMAKE_BINARY_DIR}/spp_scripts/player_v2.g2.so"
    PRE_NATIVE_V1_SO="${CMAKE_BINARY_DIR}/spp_scripts/native_std_v1.g1.so"
    PRE_NATIVE_V2_SO="${CMAKE_BINARY_DIR}/spp_scripts/native_std_v2.g2.so"
    PRE_HANDLE_V1_SO="${CMAKE_BINARY_DIR}/spp_scripts/handle_v1.g1.so"
    PRE_HANDLE_V2_SO="${CMAKE_BINARY_DIR}/spp_scripts/handle_v2.g2.so"
    PRE_HANDLE_V1B_SO="${CMAKE_BINARY_DIR}/spp_scripts/handle_v1.g3.so")
gtest_discover_tests(backend_tests)

# Experiments R3 and R6: closures that outlive a generation, and the unload.
add_executable(r3_tests tests/r3_tests.cpp example/engine/Actor.cpp example/engine/Util.cpp)
target_include_directories(r3_tests PRIVATE tests)
target_link_libraries(r3_tests PRIVATE spp_backend GTest::gtest_main)
set_target_properties(r3_tests PROPERTIES ENABLE_EXPORTS ON)
target_compile_definitions(r3_tests PRIVATE SPP_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
gtest_discover_tests(r3_tests)

# Experiment R5: worker threads, a barrier and a reload from a third thread.
add_executable(r5_tests tests/r5_tests.cpp example/engine/Actor.cpp example/engine/Util.cpp)
target_include_directories(r5_tests PRIVATE tests)
target_link_libraries(r5_tests PRIVATE spp_backend GTest::gtest_main)
set_target_properties(r5_tests PROPERTIES ENABLE_EXPORTS ON)
add_dependencies(r5_tests gen4_base)
target_compile_definitions(r5_tests PRIVATE
    GEN4_BASE_SO="$<TARGET_FILE:gen4_base>"
    GEN3_MISSING_SO="${CMAKE_BINARY_DIR}/gen3_missing.so")
gtest_discover_tests(r5_tests)

# The cache of the translator session needs only spc_lib.
add_executable(translator_tests tests/translator_tests.cpp)
target_link_libraries(translator_tests PRIVATE spc_lib GTest::gtest_main)
target_include_directories(translator_tests PRIVATE ${CMAKE_SOURCE_DIR}/src/translator/parser ${CMAKE_SOURCE_DIR}/src/translator)
target_compile_definitions(translator_tests PRIVATE SPP_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
gtest_discover_tests(translator_tests)

# The printer of the expression and statement AST.
add_executable(emit_tests tests/emit_tests.cpp)
target_link_libraries(emit_tests PRIVATE spc_lib GTest::gtest_main)
target_include_directories(emit_tests PRIVATE ${CMAKE_SOURCE_DIR}/src/translator)
gtest_discover_tests(emit_tests)

# The type model: stubs to C++ storage types.
add_executable(types_tests tests/types_tests.cpp)
target_link_libraries(types_tests PRIVATE spc_lib GTest::gtest_main)
target_include_directories(types_tests PRIVATE ${CMAKE_SOURCE_DIR}/src/translator)
gtest_discover_tests(types_tests)

# The parser of the expressions: round trip with the printer.
add_executable(expr_parse_tests tests/expr_parse_tests.cpp)
target_link_libraries(expr_parse_tests PRIVATE spc_lib GTest::gtest_main)
target_include_directories(expr_parse_tests PRIVATE ${CMAKE_SOURCE_DIR}/src/translator)
gtest_discover_tests(expr_parse_tests)

# The parser of the statements: round trip with the printer, and the example scripts.
add_executable(stmt_parse_tests tests/stmt_parse_tests.cpp)
target_link_libraries(stmt_parse_tests PRIVATE spc_lib GTest::gtest_main)
target_include_directories(stmt_parse_tests PRIVATE ${CMAKE_SOURCE_DIR}/src/translator)
target_compile_definitions(stmt_parse_tests PRIVATE SPP_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
gtest_discover_tests(stmt_parse_tests)

# The lowering pass.
add_executable(lower_tests tests/lower_tests.cpp)
target_link_libraries(lower_tests PRIVATE spc_lib GTest::gtest_main)
target_include_directories(lower_tests PRIVATE ${CMAKE_SOURCE_DIR}/src/translator)
gtest_discover_tests(lower_tests)

# The script::deref helper of the runtime header.
add_executable(deref_tests tests/deref_tests.cpp)
target_link_libraries(deref_tests PRIVATE spp_runtime GTest::gtest_main)
gtest_discover_tests(deref_tests)

# JIT only: no file, no child process, the life of a generation.
if(SPP_BACKEND STREQUAL "JIT")
    add_executable(jit_tests tests/jit_tests.cpp example/engine/Actor.cpp example/engine/Object.cpp example/engine/Util.cpp)
    target_include_directories(jit_tests PRIVATE tests)
    target_link_libraries(jit_tests PRIVATE spp_backend GTest::gtest_main)
    set_target_properties(jit_tests PROPERTIES ENABLE_EXPORTS ON)
    target_compile_definitions(jit_tests PRIVATE SPP_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
        SPP_CXX="${CMAKE_CXX_COMPILER}" SPP_RESOURCE_DIR="${CLANG_RESOURCE_DIR}")
    gtest_discover_tests(jit_tests)
endif()

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
