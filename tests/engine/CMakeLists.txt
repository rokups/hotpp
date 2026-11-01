# Copyright (c) 2026 the Hot++ project.
# This work is licensed under the terms of the MIT license.
# For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

# The tests link this library for the file watcher.
add_library(engine_harness STATIC FileWatcher.cpp)
target_include_directories(engine_harness PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
