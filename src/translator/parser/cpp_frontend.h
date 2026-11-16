// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The ".cpp" frontend. It reads a plain C++ script file with libclang.
// The result is the same ast::Script that parse_script gives for a ".hot" file.
#pragma once

#include "../ast.h"
#include "../diag.h"

#include <chrono>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace hotc
{

// Keeps libclang translation units between two parses of a script, so that libclang parses the
// include preamble (the first block of directives of the unit) only once. A unit stays in the
// cache only while no thread uses it: parse_cpp_script takes the unit out and puts it back at
// the end. So one thread at a time uses a unit, and the cache is thread safe. The key of a unit
// is the file name, the compiler arguments and the current directory. The cache keeps a bounded
// number of units and drops the oldest one. A unit that nobody used for the idle limit (default
// 10 minutes) is disposed at the next translate (or size() call); there is no thread. A unit is
// dropped (the next parse is fresh) when:
//   - the parse had an error in any file (a reparse can drop a diagnostic of a header),
//   - a reparse fails,
//   - a file that the unit included, or an include search directory (also its subdirectory for a
//     path include like "a/x.h"), has a new size, mtime, ctime or inode (a new file in an earlier
//     directory shadows a header),
//   - a file changed in the last 50 ms before the parse started (the edit can have come during
//     the parse): the unit is not kept,
//   - clear() ran during its parse.
// The stamps are read before a reparse. After it they stay when the preprocessor lines of the
// script are the same (the unit reads the same files). Limits: a shadowing file in the directory
// of an includer ("x.h" next to a header that includes "x.h") is not seen; an edit that keeps
// the size, the mtime, the ctime and the inode is not seen.
// The text of the script and of the headers in memory is always new: the reparse gets it.
class TuCache
{
public:
    explicit TuCache(size_t max_units = 4);
    ~TuCache();
    TuCache(const TuCache&) = delete;
    TuCache& operator=(const TuCache&) = delete;
    void clear(); // drops all kept units
    // The most kept units. 0 turns the cache off: each parse is plain and fresh. Excess units go.
    void set_max_units(size_t n);
    void set_idle_limit(std::chrono::milliseconds t); // for the tests
    size_t size() const; // the number of kept units
    struct Impl;
    Impl& state() { return *impl_; } // for the parser only

private:
    std::unique_ptr<Impl> impl_;
};

// Parses the ".cpp" script "text" with libclang. "name" is the file name for libclang.
// "clang_args" are the compiler arguments (language, standard, include paths).
// The bodies and the initializers stay raw C++ text. The function edits a reference only
// when the generated code cannot resolve it (a base member, "this", a module variable).
// The function adds an error to "diag" and stops at the first unsupported construct.
// It never throws for a bad script. The offsets in the errors are bytes in "text".
// "virtual_files" (path, text) are headers that exist only in memory. The caller keeps them
// alive until the function returns.
// "cache" (optional) keeps the libclang unit for the next call. A reparse of a kept unit gives the
// same script as a fresh parse.
ast::Script parse_cpp_script(const std::string& text, const std::string& name,
    const std::vector<std::string>& clang_args, const LineMap& map, Diagnostics& diag,
    const std::vector<std::pair<std::string, std::string>>& virtual_files = {},
    bool aot_optimized = false, TuCache* cache = nullptr);

} // namespace hotc
