// The lowering pass. It changes the parsed script AST into AST that the printer
// (emit.h) prints as C++. translate() in spc_lib.cpp uses this pass. The pass has the
// rules of the old token rewriter:
//  - The set "locals" has no block scope. A name stays in the set until the end of the
//    body. The pass visits the nodes in the order of the script text, so a use before a
//    declaration sees no local.
//  - A local declaration, a parameter of a fn and a parameter of a lambda add a
//    name to "locals".
//  - The script uses "." for a scope and for a member access. A name with more than one part
//    is a member chain when the first part is a local. Else it is a scope chain and the
//    parts join with "::". A member access on a value (not "this" and not "super") becomes
//    "script::deref(object).member". The helper works for a pointer and for a reference.
//  - "this.P" and a bare "P" (not a local) name the property P of the class. A read becomes
//    "Cls__get_P(self)". The statement "P = e;" becomes "Cls__set_P(self, e);". The statements
//    "P op= e;", "++P;" and "P++;" become "Cls__set_P(self, Cls__get_P(self) op (e));".
//  - "obj.P" names the property P of another object of the class. The pass has no types. So
//    the rule uses the name: P must be a property of the class. "super.P" is not a property.
//    The object becomes "script::shell_of<Cls_Shell>(obj, "Cls")". The helper checks at run
//    time that the object is a script object of the class. The C++ compiler rejects an
//    object that is not of the native base type. A read becomes "Cls__get_P(shell)".
//  - A write that gives a value uses a helper of script_runtime.h. So "a = this.P = 1" and
//    "x = (p.P += 2)" compile. "script::prop_set" gives the assigned value.
//    "script::prop_update" gives the new value. "script::prop_post" ("P++") gives the old
//    value. A compound write on another object also uses prop_update in a statement. The
//    helper evaluates the object once.
#pragma once

#include <map>
#include <set>
#include <string>
#include <vector>

#include "ast.h"
#include "diag.h"

namespace spc {

// What the lowering pass knows about the class that it works on.
struct RCtx {
    const ast::ClassDef* cls = nullptr;
    std::map<std::string, std::string> field_type;
    std::set<std::string> fns;
    // The properties of the class. A read becomes a call of get_Name. A write becomes a
    // call of set_Name. The translator makes the accessor fns before the lowering pass.
    struct PropInfo {
        bool has_get = false, has_set = false;
    };
    std::map<std::string, PropInfo> props;
    std::set<std::string> locals;     // parameters and declared locals
    bool allow_this = true;
    std::string where;  // the name of the function, for the stale call log of a lambda
    // The map gives the line and column of an error. Without a map an error has no line.
    const LineMap* map = nullptr;
};

// The context of a class: the fields and the script fns. A body or an initializer
// starts from a copy of it.
RCtx make_class_ctx(const ast::ClassDef& cd);
// Adds the parameters of a fn to the locals.
void add_params(RCtx& cx, const std::vector<ast::Param>& params);

// The number text in C++: "1.5" becomes "1.5f", "3L" becomes "int64_t{3}".
std::string rewrite_number(const std::string& n);

// Lowers the statements of a block. Each error goes to the diagnostics and the pass continues.
void lower_block(ast::BlockStmt& b, RCtx& cx, Diagnostics& diag);
// Lowers one expression. The pass can replace the node.
void lower_expr(ast::ExprP& e, RCtx& cx, Diagnostics& diag);

}  // namespace spc
