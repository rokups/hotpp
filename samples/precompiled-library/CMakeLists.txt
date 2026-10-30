# The sample precompiled-library: the build makes a script library. The program loads it.
# This file works in the Hot++ build (add_subdirectory) and as a separate project.
cmake_minimum_required(VERSION 3.20)
project(hot_sample_precompiled_library CXX)

# The Hot++ source tree. A separate build uses it. The Hot++ build already has the targets.
# An empty HOT_ROOT means: use the installed SDK (find_package(Hot), CMAKE_PREFIX_PATH).
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../CMakeLists.txt")
    set(HOT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH "The Hot++ source tree. Empty: use the installed SDK")
else()
    set(HOT_ROOT "" CACHE PATH "The Hot++ source tree. Empty: use the installed SDK")
endif()
if(NOT TARGET hot::hotc)
    if(HOT_ROOT)
        add_subdirectory(${HOT_ROOT} hot)
    else()
        find_package(Hot REQUIRED)
    endif()
endif()

# One program source, two script variants. The extension picks the script. Both variants are
# in the folder script. Each variant has its own library target and its own program.
#   sample_precompiled_hot loads the library of script/wolf.hot.
#   sample_precompiled_cpp loads the library of script/wolf.cpp.
# A call of hot_add_host takes scripts with one stem only, so each variant has its own call.
# The compile flags of the library follow the host flags.
separate_arguments(host_flags NATIVE_COMMAND "${CMAKE_CXX_FLAGS}")
enable_testing()
foreach(variant hot cpp)
    # The variant "hot" uses the extension ".hot".
    set(ext ${variant})
    if(variant STREQUAL "hot")
        set(ext hot)
    endif()
    # The program has no backend and no compiler at run time. It links only the runtime.
    # hotc builds wolf.g1.so at build time.
    set(target sample_precompiled_${variant})
    add_executable(${target} main.cpp)
    hot_add_host(TARGET ${target} MODE PRECOMPILED SCRIPTS script/wolf.${ext}
                 INCLUDES ${CMAKE_CURRENT_SOURCE_DIR} FLAGS ${host_flags} DEPENDS Creature.h)

    # HOT_SCRIPTS_<target>_LIBRARIES has the paths of the libraries.
    add_test(NAME ${target} COMMAND ${target} ${HOT_SCRIPTS_${target}_LIBRARIES})
endforeach()

# The normal toolchain compiles the .cpp script too. A .cpp script must compile unchanged.
add_library(precompiled_library_cpp_scripts_plain OBJECT script/wolf.cpp)
target_include_directories(precompiled_library_cpp_scripts_plain PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(precompiled_library_cpp_scripts_plain PRIVATE cxx_std_17)
