// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The nullability analyzer (design: doc/type-design.md, "Nullability"). The pass runs on the
// checked AST of a ".hot" class, after the const inference and before the lowering. It does not
// change the AST. It is flow sensitive: it walks the paths of each body and keeps the set of
// names that are known as non null.
//
// A value of the type "T*", "$T" or "T?" can be null. Access to a value that can be null is
// reported. Access means a member access, a call, an index, a dereference ("*p") and the call
// through the value. A comparison with "null" is never reported.
//
// Known non null: "this", "new", "&x", a name that "is T name" binds, a narrowed name, and a
// native return of a type with [nonnull].
// Narrowing: "if (p == null) return;" (also "break", "continue" and "throw"), "if (p != null)",
// the else branch of "if (p == null)", "while (p != null)", "&&", "||", "?:", "!", "is", "?.", "??".
// An assignment clears the narrowing. A field never narrows. A lambda body starts with no
// narrowing. A loop body starts with the narrowing that holds on entry and on every back edge.
// The narrowing of the "try" block does not hold in "catch".
// A "ref r = p;" to a nullable name disables the narrowing of both names. A "ref" to a
// nullable field is always an error.
//
// The pass reports only when the type text of the value is known. An unknown type gives no report.
#pragma once

#include "ast.h"
#include "diag.h"
#include "types.h"

#include <functional>
#include <string>

namespace hotc
{

struct NullOptions
{
    // True (the default): a finding is an error. False: it is a warning. The error of a "ref" to a nullable
    // field is always an error.
    bool as_error = true;
    // The script class with a name. Null: not known.
    std::function<const ast::ClassDef*(const std::string&)> find_class;
    // The module variable with a name. Null: not known.
    std::function<const ast::Field*(const std::string&)> module_var;
};

// Checks the bodies and the initializers of one class. Each finding goes to the diagnostics.
void check_nulls(
    const ast::ClassDef& cd, const TypeModel& types, const LineMap& map, Diagnostics& diag, const NullOptions& opt);

} // namespace hotc
