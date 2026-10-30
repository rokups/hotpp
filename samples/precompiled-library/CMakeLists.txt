# The sample precompiled-library: the build makes a script library. The program loads it.
# This file works in the S++ build (add_subdirectory) and as a separate project.
cmake_minimum_required(VERSION 3.20)
project(spp_sample_precompiled_library CXX)

# The S++ source tree. A separate build uses it. The S++ build already has the targets.
set(SPP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH "The S++ source tree")
if(NOT TARGET sppc)
    add_subdirectory(${SPP_ROOT} spp)
endif()

# One program source, two script variants. The extension picks the script. Both variants are
# in the folder script. Each variant has its own library target and its own program.
#   sample_precompiled_spp loads the library of script/wolf.spp.
#   sample_precompiled_cpp loads the library of script/wolf.cpp.
# A call of spp_add_scripts takes scripts with one stem only, so each variant has its own call and
# its own OUT_DIR. The compile flags of the library follow the host flags.
separate_arguments(host_flags NATIVE_COMMAND "${CMAKE_CXX_FLAGS}")
enable_testing()
foreach(variant spp cpp)
    # sppc builds wolf.g1.so at build time.
    spp_add_scripts(TARGET sample_precompiled_${variant}_scripts SCRIPTS script/wolf.${variant}
                    OUT_DIR ${CMAKE_BINARY_DIR}/spp_scripts/sample_precompiled_${variant}
                    INCLUDES ${CMAKE_CURRENT_SOURCE_DIR} FLAGS ${host_flags} DEPENDS Creature.h)

    # The program has no backend and no compiler at run time. It links only the runtime.
    set(target sample_precompiled_${variant})
    add_executable(${target} main.cpp)
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(${target} PRIVATE spp_runtime)
    # The library resolves host and runtime symbols from the executable.
    set_target_properties(${target} PROPERTIES ENABLE_EXPORTS ON)
    add_dependencies(${target} sample_precompiled_${variant}_scripts)

    # SPP_SCRIPTS_<target>_LIBRARIES has the paths of the libraries.
    add_test(NAME ${target} COMMAND ${target} ${SPP_SCRIPTS_sample_precompiled_${variant}_scripts_LIBRARIES})
endforeach()

# The normal toolchain compiles the .cpp script too. The compile uses no sppc and no spp include dir.
add_library(precompiled_library_cpp_scripts_plain OBJECT script/wolf.cpp)
target_include_directories(precompiled_library_cpp_scripts_plain PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(precompiled_library_cpp_scripts_plain PRIVATE cxx_std_17)
