// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The const inference (design: doc/type-design.md, "Const inference (v1)"). A script never
// writes "const" on a local, a field or a method. This pass runs after the checker and before the
// lowering. It sets the flags that the emitter prints as "const":
//  - "VarDeclStmt::is_const": the local has an initializer and the body never writes it.
//  - "ForOfStmt::is_const": a "ref" target that the body never writes.
//  - "Param::body_const": a parameter of a plain method that the body never writes. The signature
//    does not change. Only the body form has "const".
// A method and a field never get "const".
//
// The pass sees names only (the lowering has not run). It is conservative: when the pass is not
// sure that a use is a read, the use is a write. A write of a name is:
//  - an assignment, a compound assignment, "++", "--" or "&" on the name or on a path from it
//    ("x.f", "x[i]", "*x");
//  - "ref r = x;", "T& r = x;", "T[] s = x;" and "foreach (ref e in x)";
//  - a call "x.M()" of a member function (the function can be non-const);
//  - "x" as an argument of a call, unless the callee is a script function of the class and the
//    parameter is not a "ref" or an "out";
//  - "x[i]" (the index operator of a container can be non-const);
//  - "await x";
//  - "return x;", unless the type of "x" is a builtin scalar or a handle (a const local loses the
//    implicit move, and a move-only type then does not compile).
// The target of a handle ("$C", "T*" or a native handle class) stays mutable: "h.f = 1" and
// "h.M()" do not write "h". Only a new value of "h" is a write.
// The pass also finds the shared cells (G06). A local or a plain parameter that a lambda body writes
// (by the rules above, without the "return" rule) gets a shared cell: "VarDeclStmt::shared". A
// parameter gets a shared local with the same name and value at the start of the body. Each
// lambda gets "LambdaExpr::assigns" for the check of the lowering.
// The analysis works by name over the whole body (lambdas included). Two locals with the same name
// share the result, so a name that is written anywhere is not const anywhere in the body.
#pragma once

#include "ast.h"

#include <functional>
#include <map>
#include <set>
#include <string>

namespace hotc
{

struct ConstContext
{
    // The script function of the class (and of its script parents) with a name. Null: not known.
    std::function<const ast::Method*(const std::string&)> script_fn;
    // True when the script type text is a handle or a pointer: a value of it points to an object.
    std::function<bool(const std::string&)> is_handle;
    // True when the parameters of the method can get "const" in the body. Null: all methods
    // that are not virtual, not override, not abstract and not async.
    std::function<bool(const ast::Method&)> params_ok;
};

// The direct writes of a function body, for the checks of the operators. "roots" holds the names
// that the body assigns, increments or decrements ("x = 1", "x += 1", "x++", "this.x = 1", "x[i] = 1",
// "x.y = 1"). The value is true when the path is the bare name. A name that the body declares (a
// parameter or a local) is not in it. "calls" holds the names of the functions of the same object that
// the body calls ("F()", "this.F()"). It does not see a call of a native member function on a field.
struct DirectWrites
{
    std::map<std::string, bool> roots;
    std::set<std::string> calls;
};
DirectWrites direct_writes(const ast::Method& m);

// True for the type "T[]" (a view of mutable elements), also in the C++ form "hot::span<T>" or
// "std::span<T>". A "const T[]" and a view of const elements cannot write.
bool is_mutable_view(const std::string& type);

// Sets the flags for each body of the class: the methods, the constructors and the destructors.
void infer_const(ast::ClassDef& cd, const ConstContext& ctx);

} // namespace hotc
