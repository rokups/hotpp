cmake_minimum_required(VERSION 3.20)
project(spp CXX)

enable_testing()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
add_compile_options(-Wall -Wextra)

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

add_test(NAME r0 COMMAND minengine --r0 $<TARGET_FILE:gen1>)
add_test(NAME r1 COMMAND minengine --r1 $<TARGET_FILE:gen1> $<TARGET_FILE:gen2>
         $<TARGET_FILE:gen4_base> ${CMAKE_BINARY_DIR}/gen3_missing.so)
