# Copyright (c) 2026 the Hot++ project.
# This work is licensed under the terms of the MIT license.
# For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

# hotc is the Hot++ tool. It translates a .hot file (--emit-cpp) or compiles it ahead
# of time into a .so. The output loads in every build.
add_executable(hotc hotc.cpp)
add_executable(hot::hotc ALIAS hotc)
target_link_libraries(hotc PRIVATE hot_clang_backend)
