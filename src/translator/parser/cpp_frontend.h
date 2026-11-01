// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The ".cpp" frontend. It reads a plain C++ script file with libclang.
// The result is the same ast::Script that parse_script gives for a ".hot" file.
#pragma once

#include "../ast.h"
#include "../diag.h"

#include <string>
#include <utility>
#include <vector>

namespace hotc
{

// Parses the ".cpp" script "text" with libclang. "name" is the file name for libclang.
// "clang_args" are the compiler arguments (language, standard, include paths).
// The bodies and the initializers stay raw C++ text. The function edits a reference only
// when the generated code cannot resolve it (a base member, "this", a module variable).
// The function adds an error to "diag" and stops at the first unsupported construct.
// It never throws for a bad script. The offsets in the errors are bytes in "text".
// "virtual_files" (path, text) are headers that exist only in memory. The caller keeps them
// alive until the function returns.
ast::Script parse_cpp_script(const std::string& text, const std::string& name,
    const std::vector<std::string>& clang_args, const LineMap& map, Diagnostics& diag,
    const std::vector<std::pair<std::string, std::string>>& virtual_files = {},
    bool aot_optimized = false);

} // namespace hotc
