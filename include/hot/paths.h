// default_paths(): the default compiler, include directories and work directory of a host.
//
// A development build takes the values from the build tree (CMake definitions).
// A portable build (CMake option HOT_PORTABLE) holds no build path in the binary.
// It finds the values at run time instead. An empty text means "not found".
#pragma once

#include <string>
#include <vector>

namespace hot
{

struct DefaultPaths
{
    std::string compiler;                  // the C++ compiler. Empty if the search fails.
    std::vector<std::string> include_dirs; // the Hot++ include directories
    std::string resource_dir;              // the Clang resource directory. Empty means the compiler default.
    std::string work_dir;                  // the parent of the work directory of a process
    std::vector<std::string> cxxflags;     // the compile flags. They are the same in both modes.
};

// Portable build, in this order:
//   compiler:     the environment variable HOT_CXX, else clang++ on PATH.
//   include_dirs: the environment variable HOT_INCLUDE_DIR, else <exe_dir>/hot/include or
//                 <exe_dir>/../include, the first one that has hot/script.h.
//   resource_dir: the environment variable HOT_RESOURCE_DIR, else the output of
//                 "<compiler> -print-resource-dir". The query runs one time.
//   work_dir:     $XDG_RUNTIME_DIR/hot_work if the variable is set, else <temp directory>/hot_work-<uid>.
DefaultPaths default_paths();

// Creates the directory with mode 0700 if it does not exist. Then it checks that the directory is a
// real directory, that the current user owns it and that no other user can write to it.
// Returns an empty text if the directory is safe, else a message. On Windows it only creates it.
// The default backend of a portable build calls it for the work directory.
std::string check_work_dir(const std::string& dir);

} // namespace hot
