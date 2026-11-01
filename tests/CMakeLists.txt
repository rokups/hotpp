# Copyright (c) 2026 the Hot++ project.
# This work is licensed under the terms of the MIT license.
# For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

# The engine (Actor.h and others) is part of the tests. The runtime does not add its
# headers, so all targets below get this include dir.
include_directories(${PROJECT_SOURCE_DIR}/tests/engine)
# The tests. Each folder has its own CMakeLists.txt.
add_subdirectory(engine)
# The scripts need hotc (hot::hotc). A cross build has it only with HOT_HOST_HOTC.
if(TARGET hotc)
    add_subdirectory(scripts)
endif()
add_subdirectory(unit/runtime)
if(HOT_BUILD_TRANSLATOR)
    add_subdirectory(unit/translator)
endif()
if(NOT CMAKE_CROSSCOMPILING)
    add_subdirectory(unit/python)
endif()
if(HOT_BUILD_TRANSLATOR)
    add_subdirectory(golden)
    add_subdirectory(integration)
endif()
