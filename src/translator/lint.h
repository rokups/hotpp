// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The lint pass of a ".hot" class (doc/diagnostics.md, "Lint warnings"). The pass runs on the
// checked AST, before the lowering. It does not change the AST. It only adds warnings.
//
// 1. Dead code: a statement that follows "return", "break", "continue" or "throw" in the same block.
// 2. Unused local: a local that the body never reads. A name that starts with "_" is exempt.
//    A foreach variable, a catch variable, a structured binding and an "is T name" binding are exempt.
//    A local that holds an object for its constructor and destructor (RAII) also warns: use a "_" name.
// 3. Shadowing: a local, a lambda parameter, a foreach variable, a catch variable, a name of a
//    structured binding ("var (a, b) = e;" and "foreach (var (k, v) in c)") or an "is T name" binding
//    that hides a local, a binding or a parameter of the enclosing function. A field never counts.
//    The scope of an "is T name" binding is the condition and the "then" branch of its "if" or "while".
#pragma once

#include "ast.h"
#include "diag.h"

namespace hotc
{

// Checks the bodies of one class. Each finding goes to the warnings of the diagnostics.
void lint_class(const ast::ClassDef& cd, const LineMap& map, Diagnostics& diag);

} // namespace hotc
