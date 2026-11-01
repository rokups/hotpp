# Copyright (c) 2026 the Hot++ project.
# This work is licensed under the terms of the MIT license.
# For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

# The source libraries and tools. The targets name each other, so the order is free.
add_subdirectory(runtime)
if(HOT_BUILD_TRANSLATOR)
    add_subdirectory(translator)
    add_subdirectory(tools)
    add_subdirectory(ls)
elseif(HOT_HOST_HOTC)
    # A cross build: hotc runs on the build host. The scripts use it by the name hot::hotc.
    add_executable(hotc IMPORTED GLOBAL)
    set_target_properties(hotc PROPERTIES IMPORTED_LOCATION "${HOT_HOST_HOTC}")
    add_executable(hot::hotc ALIAS hotc)
endif()
