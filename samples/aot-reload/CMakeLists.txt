# The sample aot-reload: a program with AOT scripts that also overrides a script at run time.
# This file works in the Hot++ build (add_subdirectory) and as a separate project.
cmake_minimum_required(VERSION 3.20)
project(hot_sample_aot_reload CXX)

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

# One program source, two script variants. The extension picks the scripts. Both variants are in
# the folder script.
#   sample_aot_reload_hot: AOT script/wolf.hot, override script/wolf_override.hot.
#   sample_aot_reload_cpp: AOT script/wolf.cpp, override script/wolf_override.cpp.
# MODE AOT_RELOAD builds only script/wolf.<ext> into the binary (the library
# sample_aot_reload_<variant>_scripts). The override file is not in SCRIPTS: the program compiles
# it at run time. The program links hot::backend, not hot::aot. HOT_SCRIPT_DIR is the folder script.
enable_testing()
foreach(variant hot cpp)
    set(ext ${variant})
    set(target sample_aot_reload_${variant})
    add_executable(${target} main.cpp)
    target_compile_definitions(${target} PRIVATE HOT_SAMPLE_EXT=".${ext}")
    hot_add_host(TARGET ${target} MODE AOT_RELOAD SCRIPTS script/wolf.${ext}
                 INCLUDES ${CMAKE_CURRENT_SOURCE_DIR} DEPENDS Creature.h)

    add_test(NAME ${target} COMMAND ${target} ${CMAKE_CURRENT_SOURCE_DIR}/script)
    # The output must show the three phases in order, each with two ticks of its own code.
    set_tests_properties(${target} PROPERTIES
        PASS_REGULAR_EXPRESSION "== aot\n\\[aot\\][^\n]*\n\\[aot\\][^\n]*\n== override\n\\[override\\][^\n]*\n\\[override\\][^\n]*\n== revert\n\\[aot\\][^\n]*\n\\[aot\\][^\n]*\naot-reload: ok")
endforeach()

# The normal toolchain compiles the .cpp scripts too. A .cpp script must compile unchanged.
add_library(aot_reload_cpp_scripts_plain OBJECT script/wolf.cpp script/wolf_override.cpp)
target_include_directories(aot_reload_cpp_scripts_plain PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(aot_reload_cpp_scripts_plain PRIVATE cxx_std_17)
