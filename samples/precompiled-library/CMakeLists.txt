# The sample precompiled-library: the build makes a script library. The program loads it.
# This file works in the S++ build (add_subdirectory) and as a separate project.
cmake_minimum_required(VERSION 3.20)
project(spp_sample_precompiled_library CXX)

# The S++ source tree. A separate build uses it. The S++ build already has the targets.
set(SPP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH "The S++ source tree")
if(NOT TARGET sppc)
    add_subdirectory(${SPP_ROOT} spp)
endif()

# sppc builds wolf.g1.so at build time. The compile flags of the library follow the host flags.
separate_arguments(host_flags NATIVE_COMMAND "${CMAKE_CXX_FLAGS}")
spp_add_scripts(TARGET sample_precompiled_scripts SCRIPTS wolf.spp
                INCLUDES ${CMAKE_CURRENT_SOURCE_DIR} FLAGS ${host_flags} DEPENDS Creature.h)

# The program has no backend and no compiler at run time. It links only the runtime.
add_executable(sample_precompiled main.cpp)
target_include_directories(sample_precompiled PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(sample_precompiled PRIVATE spp_runtime)
# The library resolves host and runtime symbols from the executable.
set_target_properties(sample_precompiled PROPERTIES ENABLE_EXPORTS ON)
add_dependencies(sample_precompiled sample_precompiled_scripts)

# SPP_SCRIPTS_sample_precompiled_scripts_LIBRARIES has the paths of the libraries.
enable_testing()
add_test(NAME sample_precompiled COMMAND sample_precompiled ${SPP_SCRIPTS_sample_precompiled_scripts_LIBRARIES})
