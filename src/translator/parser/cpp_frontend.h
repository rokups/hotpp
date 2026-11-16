// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The ".cpp" frontend. It reads a plain C++ script file with libclang.
// The result is the same ast::Script that parse_script gives for a ".hot" file.
#pragma once

#include "../ast.h"
#include "../diag.h"

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
// is the file name and the compiler arguments. The cache keeps a bounded number of units and
// drops the oldest one. A unit that fails a reparse is dropped, and the next parse is fresh.
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
