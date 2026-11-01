// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The canonical form of a Windows path. A Windows path can have short 8.3 names
// ("C:\Users\RUNNER~1"), and clangd writes the long names ("C:\Users\runneradmin"). hot-ls
// compares the paths of clangd with its own paths, so both use the long names.
#pragma once

#include <string>

namespace hot::ls
{

// The path with long names in place of short names. The last part of the path can be absent:
// then the parent folder gets the long names. On other systems the path does not change.
std::string long_path(const std::string& path);

} // namespace hot::ls
