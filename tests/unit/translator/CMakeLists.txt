# Copyright (c) 2026 the Hot++ project.
# This work is licensed under the terms of the MIT license.
# For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

# Unit tests of the translator, one folder for each area.
add_subdirectory(parser)
add_subdirectory(lowering)
add_subdirectory(emit)
add_subdirectory(types)
add_subdirectory(diagnostics)
add_subdirectory(session)
add_subdirectory(translate)
add_subdirectory(vfs)
