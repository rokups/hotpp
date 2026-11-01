# Copyright (c) 2026 the Hot++ project.
# This work is licensed under the terms of the MIT license.
# For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

# Integration tests. The fixtures come first, because two test folders use them.
add_subdirectory(fixtures)
add_subdirectory(reload)
add_subdirectory(closures)
add_subdirectory(async)
add_subdirectory(threading)
add_subdirectory(backends)
add_subdirectory(aot)
add_subdirectory(ls)
