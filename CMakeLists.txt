cmake_minimum_required(VERSION 3.20)
project(spp CXX)

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
