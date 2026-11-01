# Copyright (c) 2026 the Hot++ project.
# This work is licensed under the terms of the MIT license.
# For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

# The SDK smoke test: a minimal consumer of an installed Hot++ SDK. It uses only find_package(Hot).
# Configure it with CMAKE_PREFIX_PATH=<sdk>. The script run.cmake does all the steps.
cmake_minimum_required(VERSION 3.20)
project(hot_sdk_smoke CXX)

find_package(Hot REQUIRED)

add_executable(sdk_smoke main.cpp)
hot_add_host(TARGET sdk_smoke MODE AOT SCRIPTS doubler.hot
             INCLUDES ${CMAKE_CURRENT_SOURCE_DIR} DEPENDS Calc.h)
