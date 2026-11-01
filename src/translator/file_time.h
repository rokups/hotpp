// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The times of a file on the disk, for the cache stamps of the translator.
#pragma once

#include "hot/vfs.h"

#include <cstdint>
#include <string>
#include <vector>

namespace hotc
{

struct FileTimes
{
    bool exists = false;
    uint64_t mtime_ns = 0; // the time of the last write
    uint64_t ctime_ns = 0; // the time of the last status change (Windows: the NTFS change time)
    uint64_t size = 0;
    uint64_t ino = 0; // the inode number (Windows: the file index)
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
// form of clang_path first, then each mix of "/" and "\\" at the last four separators. A path
// with ".." parts has more such separators: the include search adds "\\" before each relative
// include ("../b/y.h") too. Then the mix also covers the separators from the folder before the
// first "..", at most eight in total. On POSIX the result is clang_path(path) only.
std::vector<std::string> clang_spellings(const std::string& path);

// The path without the parts "<name>/../". The other text and the separators ("/" or "\\")
// stay. A file system in memory can have the name with a mix of separators (a Windows folder
// and "/"). std::filesystem::path::lexically_normal changes "/" to "\\" on Windows, so it does
// not find such a name. A drive ("C:"), "." and ".." are not a <name>.
std::string text_without_dot_dot(std::string path);

// The other names to try in a host file system for a path of the include search, when the
// path itself is not found: the lexically normal path, then text_without_dot_dot(path). Each
// name is in the list once, and the path is not in the list.
std::vector<std::string> host_path_alternatives(const std::string& path);

// The host file system can hold a file under the normal spelling ("d/b/y.h") of the path that
// libclang uses ("d/a/../b/y.h"). These two functions try the path first. Only after a miss,
// they compute and try host_path_alternatives(path) in order.
hot::VfsStat host_stat(hot::FileSystem& fs, const std::string& path);
bool host_read(hot::FileSystem& fs, const std::string& path, std::string& out);

} // namespace hotc
