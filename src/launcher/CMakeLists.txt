# Copyright (c) 2026 the Hot++ project.
# This work is licensed under the terms of the MIT license.
# For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

# The launcher library (include/hot/launcher.h): load an entry script and run its main.
# It needs a run-time backend. The caller (src/CMakeLists.txt) checks the target hot_backend.
add_library(hot_launcher STATIC launcher.cpp)
target_link_libraries(hot_launcher PUBLIC hot_backend)
add_library(hot::launcher ALIAS hot_launcher)
