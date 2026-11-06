# Copyright (c) 2026 the Hot++ project.
# This work is licensed under the terms of the MIT license.
# For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

# The program of the lldb session test: an AOT build of target.hot with debug info.
# It is not part of "all". The ctest fixture PythonLldbTarget builds it (see ../CMakeLists.txt).
# The caller guards this folder with NOT MSVC: the flags below are for GCC and Clang.
add_executable(lldb_target EXCLUDE_FROM_ALL main.cpp)
# NO_OPTIMIZE: the lldb formatters do not support the optimized AOT layout (HOT_SCRIPT_MODE
# AOT_OPTIMIZED), so this host stays plain AOT in every mode.
hot_add_host(TARGET lldb_target MODE AOT NO_OPTIMIZE SCRIPTS target.hot
             INCLUDES ${CMAKE_CURRENT_SOURCE_DIR} DEPENDS Npc.h)
# The debugger needs debug info and no optimization in the script code too, in every configuration.
foreach(_t lldb_target lldb_target_scripts)
    if(TARGET ${_t})
        target_compile_options(${_t} PRIVATE -g -O0)
        set_target_properties(${_t} PROPERTIES EXCLUDE_FROM_ALL TRUE)
    endif()
endforeach()
