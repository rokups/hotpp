cmake_minimum_required(VERSION 3.20)
project(spp CXX)

enable_testing()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
add_compile_options(-Wall -Wextra)

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

# Script runtime: a small static library for the executable.
add_library(spp_runtime STATIC runtime/script_runtime.cpp)
target_include_directories(spp_runtime PUBLIC runtime engine)
target_link_libraries(spp_runtime PUBLIC ${CMAKE_DL_LIBS})

# Engine harness. ENABLE_EXPORTS adds -rdynamic, so a generation module
# resolves the engine symbols and the runtime symbols from the executable.
add_executable(minengine engine/main.cpp engine/Actor.cpp)
target_link_libraries(minengine PRIVATE spp_runtime)
set_target_properties(minengine PROPERTIES ENABLE_EXPORTS ON)

# Step R0: generation 1 of the Player class.
add_library(gen1 SHARED poc/r0/gen1.cpp)
target_include_directories(gen1 PRIVATE runtime engine)
set_target_properties(gen1 PROPERTIES PREFIX "")

# Step R1: generation 2 (migration) and generation 4 (rejected: other shell).
# gen3_bad.cpp does not compile on purpose. It is not a target.
# See poc/r1/make_bad.sh.
foreach(g gen2:poc/r1/gen2.cpp gen4_base:poc/r1/gen4_base.cpp)
    string(REPLACE ":" ";" parts ${g})
    list(GET parts 0 gname)
    list(GET parts 1 gsrc)
    add_library(${gname} SHARED ${gsrc})
    target_include_directories(${gname} PRIVATE runtime engine)
    set_target_properties(${gname} PROPERTIES PREFIX "")
endforeach()

# GoogleTest cases R0 and R1. The file gen3_missing.so does not exist on purpose.
foreach(r r0 r1)
    add_executable(${r}_tests tests/${r}_tests.cpp engine/Actor.cpp)
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
# The translator spc. It needs libclang (C API) from the system.
find_library(CLANG_C_LIBRARY NAMES clang REQUIRED)
find_path(CLANG_C_INCLUDE_DIR clang-c/Index.h REQUIRED)
execute_process(COMMAND ${CMAKE_CXX_COMPILER} -print-resource-dir
                OUTPUT_VARIABLE CLANG_RESOURCE_DIR OUTPUT_STRIP_TRAILING_WHITESPACE)
add_executable(spc translator/spc.cpp)
target_include_directories(spc PRIVATE ${CLANG_C_INCLUDE_DIR})
target_link_libraries(spc PRIVATE ${CLANG_C_LIBRARY})
target_compile_definitions(spc PRIVATE SPC_RESOURCE_DIR="${CLANG_RESOURCE_DIR}")

# Two generations of scripts/player_*.script. spc makes the .cpp files. The two
# calls share one slot file, so the second call depends on the first.
set(GEN_DIR ${CMAKE_BINARY_DIR}/generated)
set(SPC_ARGS -I ${CMAKE_SOURCE_DIR}/engine -I ${CMAKE_SOURCE_DIR}/runtime)
set(SPC_DEPS spc ${CMAKE_SOURCE_DIR}/engine/Actor.h ${CMAKE_SOURCE_DIR}/engine/Math.h
             ${CMAKE_SOURCE_DIR}/runtime/script_runtime.h)
add_custom_command(
    OUTPUT ${GEN_DIR}/player_v1.cpp
    COMMAND ${CMAKE_COMMAND} -E make_directory ${GEN_DIR}
    COMMAND spc ${CMAKE_SOURCE_DIR}/scripts/player_v1.script --gen 1 -o ${GEN_DIR}/player_v1.cpp
            --slots ${GEN_DIR}/player.slots ${SPC_ARGS}
    DEPENDS ${SPC_DEPS} ${CMAKE_SOURCE_DIR}/scripts/player_v1.script
    VERBATIM)
add_custom_command(
    OUTPUT ${GEN_DIR}/player_v2.cpp
    COMMAND spc ${CMAKE_SOURCE_DIR}/scripts/player_v2.script --gen 2 -o ${GEN_DIR}/player_v2.cpp
            --slots ${GEN_DIR}/player.slots ${SPC_ARGS}
    DEPENDS ${SPC_DEPS} ${CMAKE_SOURCE_DIR}/scripts/player_v2.script ${GEN_DIR}/player_v1.cpp
    VERBATIM)
foreach(g v1 v2)
    add_library(f1_${g} SHARED ${GEN_DIR}/player_${g}.cpp)
    target_include_directories(f1_${g} PRIVATE runtime engine)
    set_target_properties(f1_${g} PROPERTIES PREFIX "")
endforeach()

# GoogleTest cases: F1Reload (generated libraries) and F1Diag (error at the script).
add_executable(f1_tests tests/f1_tests.cpp engine/Actor.cpp)
target_include_directories(f1_tests PRIVATE tests)
target_link_libraries(f1_tests PRIVATE spp_runtime GTest::gtest_main)
set_target_properties(f1_tests PROPERTIES ENABLE_EXPORTS ON)
add_dependencies(f1_tests f1_v1 f1_v2 spc)
target_compile_definitions(f1_tests PRIVATE
    F1_V1_SO="$<TARGET_FILE:f1_v1>"
    F1_V2_SO="$<TARGET_FILE:f1_v2>"
    SPP_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    SPP_BINARY_DIR="${CMAKE_BINARY_DIR}"
    SPC_PATH="$<TARGET_FILE:spc>"
    SPP_CXX="${CMAKE_CXX_COMPILER}"
    SPP_CXXFLAGS="${CMAKE_CXX_FLAGS}")
gtest_discover_tests(f1_tests)
