// The lowering pass. It changes the parsed script AST into AST that the printer
// (emit.h) prints as C++. translate() in spc_lib.cpp uses this pass. The pass has the
// rules of the old token rewriter:
//  - The sets "locals" and "ptr_names" have no block scope. A name stays in the set
//    until the end of the body. The pass visits the nodes in the order of the script
//    text, so a use before a declaration sees no local.
//  - A let or const declaration adds the name to "locals". It does not add the name
//    to "ptr_names". Only a parameter of a fn and of a lambda can add a name there.
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
    std::set<std::string> ptr_names;  // fields and parameters of pointer type
    std::set<std::string> locals;     // parameters and declared locals
    bool allow_this = true;
    std::string where;  // the name of the function, for the stale call log of a lambda
    // The map gives the line and column of an error. Without a map an error has no line.
    const LineMap* map = nullptr;
};

// The context of a class: the fields and the script fns. A body or an initializer
// starts from a copy of it.
RCtx make_class_ctx(const ast::ClassDef& cd);
// Adds the parameters of a fn to the locals. A parameter of pointer type is a pointer name.
void add_params(RCtx& cx, const std::vector<ast::Param>& params);

// The number text in C++: "1.5" becomes "1.5f", "3L" becomes "int64_t{3}".
std::string rewrite_number(const std::string& n);

// Lowers the statements of a block. Each error goes to the diagnostics and the pass continues.
void lower_block(ast::BlockStmt& b, RCtx& cx, Diagnostics& diag);
// Lowers one expression. The pass can replace the node.
void lower_expr(ast::ExprP& e, RCtx& cx, Diagnostics& diag);

}  // namespace spc
