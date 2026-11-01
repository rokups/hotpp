# Copyright (c) 2026 the Hot++ project.
# This work is licensed under the terms of the MIT license.
# For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

# The engine (Actor.h and others) is part of the tests. The runtime does not add its
# headers, so all targets below get this include dir.
include_directories(${PROJECT_SOURCE_DIR}/tests/engine)
# The tests. Each folder has its own CMakeLists.txt.
add_subdirectory(engine)
add_subdirectory(scripts)
add_subdirectory(unit/runtime)
add_subdirectory(unit/translator)
add_subdirectory(unit/python)
add_subdirectory(golden)
add_subdirectory(integration)
