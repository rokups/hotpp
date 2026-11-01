// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The times of a file on the disk, for the cache stamps of the translator.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace hotc
{

struct FileTimes
{
    bool exists = false;
    uint64_t mtime_ns = 0; // the time of the last write
    uint64_t ctime_ns = 0; // the time of the last status change (Windows: the last write)
    uint64_t size = 0;
    uint64_t ino = 0; // the inode number (Windows: 0)
};

// The times of the file at path. A missing file gives exists == false and zero values.
FileTimes file_times(const std::string& path);

// A path in the form for a libclang argument. On Windows, libclang does not find a path with a
// root folder and no drive ("/home/x", for example under wine). This function adds the current
// drive to such a path and gives all paths with "/" as the separator. On POSIX the path does
// not change.
std::string clang_path(const std::string& path);

// The names of an unsaved file (a header in memory) for libclang. libclang finds an unsaved file
// only by the exact name. On Windows, the include search adds "\\" between the folder and the
// name in the "#include" line, so the name has more than one form. This function gives the
// form of clang_path first, then each mix of "/" and "\\" at the last four separators. On POSIX
// the result is clang_path(path) only.
std::vector<std::string> clang_spellings(const std::string& path);

} // namespace hotc
