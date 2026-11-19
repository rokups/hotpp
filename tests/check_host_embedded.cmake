# Copyright (c) 2026 the Hot++ project.
# This work is licensed under the terms of the MIT license.
# For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

# The test host_embedded: hot_script_host writes the "embedded" array of the host JSON file.
#   cmake -DROOT=<the source tree> -DWORK=<a work dir> -P check_host_embedded.cmake
cmake_minimum_required(VERSION 3.20)
if(NOT ROOT OR NOT WORK)
    message(FATAL_ERROR "check_host_embedded.cmake needs -DROOT and -DWORK")
endif()

function(configure_case name result text)
    set(bin "${WORK}/${name}")
    file(REMOVE_RECURSE "${bin}")
    execute_process(
        COMMAND ${CMAKE_COMMAND} -S "${ROOT}/tests/host_embedded" -B "${bin}" -DROOT=${ROOT} -DCASE=${name}
        RESULT_VARIABLE code OUTPUT_QUIET ERROR_VARIABLE err)
    set(${result} "${code}" PARENT_SCOPE)
    set(${text} "${err}" PARENT_SCOPE)
    if(code EQUAL 0)
        file(READ "${bin}/hosts/H.json" json)
        set(${text} "${json}" PARENT_SCOPE)
    endif()
endfunction()

configure_case(plain code json)
if(NOT code EQUAL 0 OR json MATCHES "\"embedded\"")
    message(FATAL_ERROR "Without EMBEDDED_SUFFIX the host has no embedded key: ${code} ${json}")
endif()

set(src "${ROOT}/tests/host_embedded")
configure_case(rule code json)
set(expect "\"embedded\": [{\"suffix\": \".rml\", \"tag\": \"script\", \"prefixFile\": \"sub/p.hot\"}]")
string(FIND "${json}" "${expect}" pos)
if(NOT code EQUAL 0 OR pos EQUAL -1)
    message(FATAL_ERROR "The embedded rule is wrong. Expected ${expect} in: ${code} ${json}")
endif()

configure_case(tag code json)
set(expect "\"embedded\": [{\"suffix\": \".x\", \"tag\": \"code\", \"prefixFile\": \"p.hot\"}]")
string(FIND "${json}" "${expect}" pos)
if(NOT code EQUAL 0 OR pos EQUAL -1)
    message(FATAL_ERROR "The embedded tag is wrong. Expected ${expect} in: ${code} ${json}")
endif()

configure_case(outside code json)
set(expect "\"prefixFile\": \"${ROOT}/tests/check_host_embedded.cmake\"}]")
string(FIND "${json}" "${expect}" pos)
if(NOT code EQUAL 0 OR pos EQUAL -1)
    message(FATAL_ERROR "A prefix file outside sourceDir must stay absolute. Expected ${expect} in: ${code} ${json}")
endif()

configure_case(noprefix code text)
if(code EQUAL 0 OR NOT text MATCHES "EMBEDDED_SUFFIX needs EMBEDDED_PREFIX_FILE")
    message(FATAL_ERROR "A suffix without a prefix file must be a configure error: ${code} ${text}")
endif()
message(STATUS "The embedded rule of the host is correct")
