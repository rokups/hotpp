# Copyright (c) 2026 the Hot++ project.
# This work is licensed under the terms of the MIT license.
# For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

# A small project for the test host_embedded. The variable CASE selects the call of hot_script_host.
cmake_minimum_required(VERSION 3.20)
project(HostEmbedded NONE)
set(HOT_HOSTS_DIR "${CMAKE_BINARY_DIR}/hosts" CACHE PATH "")
include("${ROOT}/cmake/HotFormat.cmake")
include("${ROOT}/cmake/HotScripts.cmake")
if(CASE STREQUAL "plain")
    hot_script_host(NAME H SCRIPTS a.hot)
elseif(CASE STREQUAL "rule")
    hot_script_host(NAME H SCRIPTS a.hot EMBEDDED_SUFFIX .rml EMBEDDED_PREFIX_FILE sub/p.hot)
elseif(CASE STREQUAL "tag")
    hot_script_host(NAME H SCRIPTS a.hot EMBEDDED_SUFFIX .x EMBEDDED_TAG code
        EMBEDDED_PREFIX_FILE "${CMAKE_CURRENT_SOURCE_DIR}/p.hot")
elseif(CASE STREQUAL "noprefix")
    hot_script_host(NAME H SCRIPTS a.hot EMBEDDED_SUFFIX .rml)
endif()
