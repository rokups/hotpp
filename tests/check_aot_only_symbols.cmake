# Copyright (c) 2026 the Hot++ project.
# This work is licensed under the terms of the MIT license.
# For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

cmake_minimum_required(VERSION 3.20)

# The check of an AOT-only build (HOT_FORCE_AOT_ONLY) on Linux. The runtime library and the AOT
# program must not need a loader, a process start, a file watch, Clang or LLVM.
# Input: -DNM=<nm> -DLDD=<ldd> -DRUNTIME_LIB=<libhot_runtime.a> -DAOT_DEMO=<binary>
#        -DAOT_OBJECTS=<list of object or library files, optional>
# Each file is read with nm --undefined-only (with -D for a binary). The check fails when an
# undefined or weak undefined name is one of: dlopen, dlsym, dlclose, dlmopen, dladdr, fork,
# vfork, clone, posix_spawn*, exec*, fexecve, popen, pclose, system, inotify*, or a Clang or LLVM
# name (clang_*, LLVM*, the C++ namespaces clang and llvm). The ldd output of the binary must have no libclang
# and no LLVM.

set(_bad_exact "dlopen" "dlsym" "dlclose" "dlmopen" "dladdr" "fork" "vfork" "clone" "__libc_fork"
    "__libc_dlopen_mode" "posix_spawn" "posix_spawnp" "fexecve" "popen" "pclose" "system")
set(_failed FALSE)

function(check_undefined file dynamic)
    set(_args --undefined-only)
    if(dynamic)
        list(PREPEND _args -D)
    endif()
    execute_process(COMMAND ${NM} ${_args} ${file}
        RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    if(NOT _rc EQUAL 0)
        message(SEND_ERROR "nm failed on ${file}: ${_err}")
        return()
    endif()
    string(REPLACE "\n" ";" _lines "${_out}")
    set(_found "")
    foreach(_line IN LISTS _lines)
        if(NOT _line MATCHES "^[ \t]*[UwV][ \t]+([^ \t]+)")
            continue()
        endif()
        set(_name "${CMAKE_MATCH_1}")
        string(REGEX REPLACE "@.*$" "" _base "${_name}")
        if(_base IN_LIST _bad_exact OR _base MATCHES "^exec(l|le|lp|v|ve|vp|vpe|veat)?$"
           OR _base MATCHES "^inotify" OR _base MATCHES "^posix_spawn_file_actions_"
           OR _base MATCHES "^(clang_|LLVM|_ZN[K]?[0-9]*(clang|llvm))|libclang|libLLVM")
            list(APPEND _found "${_name}")
        endif()
    endforeach()
    if(_found)
        list(REMOVE_DUPLICATES _found)
        message(SEND_ERROR "${file} needs forbidden symbols: ${_found}")
    else()
        message(STATUS "ok: ${file} has no forbidden undefined symbol")
    endif()
endfunction()

check_undefined(${RUNTIME_LIB} FALSE)
foreach(_file IN LISTS AOT_OBJECTS)
    check_undefined(${_file} FALSE)
endforeach()
check_undefined(${AOT_DEMO} TRUE)

execute_process(COMMAND ${LDD} ${AOT_DEMO} RESULT_VARIABLE _rc OUTPUT_VARIABLE _ldd ERROR_VARIABLE _err)
if(NOT _rc EQUAL 0)
    message(SEND_ERROR "ldd failed on ${AOT_DEMO}: ${_err}")
elseif(_ldd MATCHES "[Cc][Ll][Aa][Nn][Gg]|[Ll][Ll][Vv][Mm]")
    message(SEND_ERROR "ldd of ${AOT_DEMO} lists libclang or LLVM:\n${_ldd}")
else()
    message(STATUS "ok: ldd of ${AOT_DEMO} has no libclang and no LLVM")
endif()
