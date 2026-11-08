# Copyright (c) 2026 the Hot++ project.
# This work is licensed under the terms of the MIT license.
# For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

# The engine (Actor.h and others) is part of the tests. The runtime does not add its
# headers, so all targets below get this include dir.
include_directories(${PROJECT_SOURCE_DIR}/tests/engine)
# The test code sees the script mode of the build (HOT_SCRIPT_MODE). __HOTPP_RELOAD__ lets a
# test compile only when the build reloads scripts at run time. __HOTPP_AOT_OPTIMIZED__
# marks AOT_OPTIMIZED: the AOT code has no slots, so an override of an AOT unit fails.
if(HOT_SCRIPT_AOT)
    add_compile_definitions(__HOTPP_AOT__)
    if(HOT_SCRIPT_AOT_OPTIMIZED)
        add_compile_definitions(__HOTPP_AOT_OPTIMIZED__)
    endif()
else()
    add_compile_definitions(__HOTPP_RELOAD__)
endif()
# Only HOT_EXPORT exports a symbol. The test fails when a CMake file exports all symbols.
add_test(NAME no_export_all_symbols
    COMMAND ${CMAKE_COMMAND} -DROOT=${PROJECT_SOURCE_DIR} -P ${CMAKE_CURRENT_SOURCE_DIR}/check_no_export_all.cmake)
# A script library that hotc does not build allows undefined symbols on macOS only.
add_test(NAME module_link_options
    COMMAND ${CMAKE_COMMAND} -DROOT=${PROJECT_SOURCE_DIR} -P ${CMAKE_CURRENT_SOURCE_DIR}/check_module_link.cmake)
# The tests. Each folder has its own CMakeLists.txt.
add_subdirectory(engine)
# The scripts need hotc (hot::hotc). A cross build has it only with HOT_HOST_HOTC.
if(TARGET hotc)
    add_subdirectory(scripts)
endif()
add_subdirectory(unit/runtime)
if(HOT_BUILD_TRANSLATOR)
    add_subdirectory(unit/translator)
endif()
if(NOT CMAKE_CROSSCOMPILING)
    add_subdirectory(unit/python)
endif()
if(HOT_BUILD_TRANSLATOR)
    add_subdirectory(golden)
    add_subdirectory(integration)
endif()
