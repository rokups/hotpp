# The sample aot: a program with the C++ of a script inside the binary.
# This file works in the S++ build (add_subdirectory) and as a separate project.
cmake_minimum_required(VERSION 3.20)
project(spp_sample_aot CXX)

# The S++ source tree. A separate build uses it. The S++ build already has the targets.
set(SPP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH "The S++ source tree")
if(NOT TARGET sppc)
    add_subdirectory(${SPP_ROOT} spp)
endif()

# sppc translates wolf.spp to C++. The library sample_aot_scripts has the result and links spp_aot.
spp_add_scripts(TARGET sample_aot_scripts AOT SCRIPTS wolf.spp
                INCLUDES ${CMAKE_CURRENT_SOURCE_DIR} DEPENDS Creature.h)
add_executable(sample_aot main.cpp)
target_include_directories(sample_aot PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(sample_aot PRIVATE sample_aot_scripts)

enable_testing()
add_test(NAME sample_aot COMMAND sample_aot)
