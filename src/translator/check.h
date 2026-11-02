// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The checks of the script AST that need no libclang (design 5.3, 7.10.4 and 7.10.6).
// The pass runs on the parsed AST, before the lowering. It does not change the AST.
//
// It reports:
//  - a field, a property, a return type or a collection element that holds a borrowed
//    pointer or reference: "T*" or "T&" of a type that is not a handle,
//  - a lambda that captures a "T&" parameter or a local that holds a borrowed
//    pointer or reference,
//  - a "&e" that the script stores: the right side of an assignment, a "return" or a
//    field initializer,
//  - a script function used as a value (it has no address, because a reload replaces it).
//
// The pass sees the types that the script writes. A "var" local from a native call or member
// gets its type from libclang (VarDeclStmt::native_type, S20b).
#pragma once

#include "ast.h"
#include "diag.h"
#include "types.h"

#include <functional>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace hotc
{

// Checks one class. Each error goes to the diagnostics. The pass continues after an error.
// The findings of the borrowed-storage rule are errors with "strict_borrowed". Otherwise they
// are warnings. The rule on the address of a script function is always an error. "is_cpp" is
// true for a class of a ".cpp" script: it can hold a raw pointer to a script class.
// "std_below_20" is true when the C++ standard of the build is older than C++20: an "async"
// method is then an error, because it needs coroutines.
void check_class(const ast::ClassDef& cd, const TypeModel& types, const LineMap& map, Diagnostics& diag,
    bool strict_borrowed, bool is_cpp = false, bool std_below_20 = false);

// Checks the "[value] T" uses of a script: fields, parameters, return types and locals (design
// 7.10). "[value]" holds a script "class" with value semantics. The class must have no native base.
// A native handle type, a struct and a builtin type are errors. A "[value] T x" without an
// initializer needs a constructor of T with no parameters. A field of a struct and a module
// variable take no "[value]" yet. A template argument has no syntax for "[value]" yet. Each
// error goes to the diagnostics. The pass does not change the AST.
void check_value_uses(
    const std::vector<ast::ClassDef>& classes, const TypeModel& types, const LineMap& map, Diagnostics& diag);

// True when the standard option ("c++17", "gnu++14", "c++2b") names a standard older than C++20.
// An empty or unknown text gives false.
bool cxx_std_below_20(const std::string& std_name);

// Checks the constructors of a class: no two overloads with the same parameter types, and the
// arguments of "base(...)" use the constructor parameters only (design 7.3). Each error goes
// to the diagnostics.
void check_ctors(const ast::ClassDef& cd, const LineMap& map, Diagnostics& diag);
// A subclass calls a constructor of its script parent with "base(...)", or implicitly. A private
// constructor of the parent is an error. A protected one is allowed.
void check_base_ctor_access(
    const ast::ClassDef& cd, const ast::ClassDef& parent, const LineMap& map, Diagnostics& diag);

// The type names that a "new" expression names, in all classes of the script. The translator
// emits a "new" function for each script class in the set.
std::set<std::string> new_targets(const ast::Script& sc);
// The classes that a "new" in the block of the class builds: a field initializer or a base
// argument. The block needs the "new" function of each one, so the class comes after them.
std::set<std::string> block_new_targets(const ast::ClassDef& cd);

// The C++ type names that the script writes in declarations, "new" and lambda parameters.
// The translator asks libclang about them when the stub "class *;" is active.
std::set<std::string> used_type_names(const ast::Script& sc);
// The same for one class. The translator uses it to find the script classes that a class names.
std::set<std::string> used_type_names(const ast::ClassDef& cd);

// How the body of a script function uses one parameter (design 7.10.5). The pass sees the
// names only, because the lowering has not run. It is conservative:
//  - "mutated": the body assigns to the parameter or to a member of it, applies "++" or "--",
//    passes it to a "T&" parameter or takes "&" of it, or calls a member function on it (the function can be
//    non-const).
//  - "rebound": the body assigns to the whole parameter, applies "++" or "--" to it, or passes
//    it to a "T&" parameter or takes "&" of it. For a handle this is the only change that needs a copy.
//  - "stored": the body assigns the parameter as a whole (right side of "="), returns it,
//    passes it to "new", or uses it in a lambda (a capture keeps a copy).
//
// The context makes the analysis deeper (S20b). All parts are optional:
//  - "script_fn" finds a script function of the class by name. A parameter that the body passes
//    by value to a script function gets the use of the parameter of that function.
//  - "fields" has the fields of the class (and of its script parents) with their script types.
//    A parameter of the type T can alias a field of the type T or a field with T in its template
//    arguments. When the function (or a script function that it calls) writes such a field, the
//    parameter is "aliased": the generation function takes a copy (a value and a handle).
//  - "const_method" is true when each member function of the name on the native type is const.
//    A call "p.M()" of such a function does not count as a change.
struct ParamUse
{
    bool mutated = false, rebound = false, stored = false, aliased = false;
};
// More parts of the context (S25). The analysis follows a parameter that a call gets:
//  - "call_target" says what a call with a parameter as an argument calls, when it is not a
//    script function of the class (S25): a method of another script class (a call on a handle),
//    or a native function by its key. The call is the node of the AST. Null: not known.
//  - "native_arg" gives how the native function "key" takes the argument "arg" of "argc": 'c' a
//    "const T&" (no change, no store), 'm' a "T&", "T&&" or "T*" (a change, may store), 's' a
//    value or any other form (may store). 's' also means: not known (the conservative answer).
//  - "for_class" gives the context of another script class. A call on a handle uses it, so a
//    bare call in the body of the callee finds the functions of the callee class.
struct CallTarget
{
    const ast::ClassDef* script = nullptr; // the static class of the receiver, with "name"
    std::string name; // the method name for "script"
    std::string native; // the key of the native function, e.g. "ns::f"
};
// The finished results by method and parameter name. The context shares one memo for the script.
using ParamMemo = std::map<std::pair<const ast::Method*, std::string>, ParamUse>;
struct ParamContext
{
    ParamMemo* memo = nullptr;
    std::function<const ast::Method*(const std::string&)> script_fn;
    std::map<std::string, std::string> fields;
    std::function<bool(const std::string& type, const std::string& method)> const_method;
    std::function<const CallTarget*(const ast::CallExpr&)> call_target;
    std::function<char(const std::string& key, size_t arg, size_t argc)> native_arg;
    std::function<ParamContext(const ast::ClassDef&)> for_class;
};
ParamUse analyze_param_use(const ast::Method& m, const std::string& name, const ParamContext& ctx = {});

// The questions for libclang about native types (S20b, design 7.10.4 and 7.10.5).
// A query is the text of a C++ type for the probe of libclang. The probe has the helpers
// "hotc_ret_f" (the return type of a function pointer or of a member function pointer) and
// "hotc_obj<T>" (the class of an object, of a pointer or of a handle with "element_type").
//  - "vars": each "var" local whose initializer is a native call, a native member or a chain of
//    them, with the query of the type. A chain uses the query of an earlier "var" local.
//  - "methods": for each native type of a value parameter, the names of the member functions
//    that a body calls on the parameter ("p.M()"). The translator asks if they are const.
// A type that names a script class or a script enum is not a query: libclang does not know it.
//  - "calls": for each call that has a parameter of the function as an argument, what it calls
//    (S25). "fn_keys" has the keys of the native functions. The translator asks libclang for
//    their parameters.
struct NativeQueries
{
    std::vector<std::pair<ast::VarDeclStmt*, std::string>> vars;
    std::map<std::string, std::set<std::string>> methods;
    std::map<const ast::CallExpr*, CallTarget> calls;
    std::set<std::string> fn_keys;
};
NativeQueries collect_native_queries(ast::Script& sc);

// True when the class uses "this" on its own, not as the object of a member access. Such a
// class can need a handle for "this" (design 7.10.1). Its shell then derives from
// std::enable_shared_from_this.
bool uses_bare_this(const ast::ClassDef& cd);

} // namespace hotc
