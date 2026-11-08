# Copyright (c) 2026 the Hot++ project.
# This work is licensed under the terms of the MIT license.
# For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

# The test module_link_options: the link options of a script library that is not made by hotc.
# Darwin has the option that allows undefined symbols. Linux and Windows have no option.
#   cmake -DROOT=<the source tree> -P check_module_link.cmake
cmake_minimum_required(VERSION 3.20)
if(NOT ROOT)
    message(FATAL_ERROR "check_module_link.cmake needs -DROOT=<the source tree>")
endif()
include("${ROOT}/cmake/HotModuleLink.cmake")

hot_module_link_options(darwin SYSTEM Darwin)
if(NOT darwin STREQUAL "LINKER:-undefined,dynamic_lookup")
    message(FATAL_ERROR "Darwin must allow undefined symbols. The options are: '${darwin}'")
endif()
foreach(system Linux Windows Android)
    hot_module_link_options(other SYSTEM ${system})
    if(other)
        message(FATAL_ERROR "${system} must have no link option. The options are: '${other}'")
    endif()
endforeach()
# The fixtures must use the helper.
file(READ "${ROOT}/tests/integration/fixtures/CMakeLists.txt" fixtures)
if(NOT fixtures MATCHES "hot_allow_undefined_symbols\\(\\$\\{name\\}\\)")
    message(FATAL_ERROR "tests/integration/fixtures/CMakeLists.txt must call hot_allow_undefined_symbols")
endif()
message(STATUS "The link options of the script libraries are correct")
