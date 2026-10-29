// The lowering pass. It changes the parsed script AST into AST that the printer
// (emit.h) prints as C++. translate() in sppc_lib.cpp uses this pass. The pass has the
// rules of the old token rewriter:
//  - The set "locals" has no block scope. A name stays in the set until the end of the
//    body. The pass visits the nodes in the order of the script text, so a use before a
//    declaration sees no local.
//  - A local declaration, a parameter of a fn and a parameter of a lambda add a
//    name to "locals".
//  - The script uses "." for a scope and for a member access. A name with more than one part
//    is a member chain when the first part is a local. Else it is a scope chain and the
//    parts join with "::". A member access on a value (not "this" and not "super") becomes
//    "spp::deref(object).member". The helper works for a pointer and for a reference.
//  - "this.P" and a bare "P" (not a local) name the property P of the class. A read becomes
//    "Cls__get_P(self)". The statement "P = e;" becomes "Cls__set_P(self, e);". The statements
//    "P op= e;", "++P;" and "P++;" become "Cls__set_P(self, Cls__get_P(self) op (e));".
//  - "obj.P" names the property P of another object of the class. A known type of obj decides
//    (S20b): a script class gives its accessors, a native type gives the native member. The
//    native base of the family and an unknown type keep the name rule: P must be a property
//    of the class. "super.P" is not a property.
//    The object becomes "spp::shell_of<Cls_Shell>(obj, "Cls")". The helper checks at run
//    time that the object is a script object of the class. The C++ compiler rejects an
//    object that is not of the native base type. A read becomes "Cls__get_P(shell)".
//  - A write that gives a value uses a helper of runtime.h. So "a = this.P = 1" and
//    "x = (p.P += 2)" compile. "spp::prop_set" gives the assigned value.
//    "spp::prop_update" gives the new value. "spp::prop_post" ("P++") gives the old
//    value. A compound write on another object also uses prop_update in a statement. The
//    helper evaluates the object once.
//  - Type-directed access. When the type of "obj" is a script class C (see RCtx), "obj.f" becomes
//    "state_of(spp::shell_of<C_Shell>(obj, "C"))->f" and "obj.M(a)" becomes
//    "C__M(spp::shell_of<C_Shell>(obj, "C"), a)". The helper throws std::runtime_error for a
//    null handle. A bare "this" becomes "spp::handle_from_this<H>(self)" where a handle type is
//    expected: a "return", an initializer, an assignment and an argument of a script function.
#pragma once

#include <map>
#include <set>
#include <string>
#include <vector>

#include "ast.h"
#include "diag.h"
#include "types.h"

namespace sppc {

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
    // The type model. With null, the pass keeps the types as the script writes them.
    const TypeModel* types = nullptr;
    // The names of all classes of the script.
    std::set<std::string> script_classes;
    // The script classes that "new" can build now: the class itself and the classes that the
    // translator emitted before it. "new C(args)" of such a class becomes "C__new(args)".
    std::set<std::string> new_classes;
    // Type information for the member access on a script object (design 7.10.1). The pass has
    // no full type check. It knows only these types, as the script writes them:
    //  - "classes": all script classes by name (with the script types of their members),
    //  - "local_type": the declared type of a parameter or a local, and the type of a "var"
    //    local that starts with "new C(...)" of a script class or with a typed member access,
    //  - "ret_type": the script return type of the function that the pass lowers.
    // With the type of "obj" known as a script class C that is not a [value] class, "obj.f"
    // reads the field of C, "obj.M(args)" calls the script function M of C and "obj.P" uses
    // the property P of C. If the type is unknown, "obj.P" keeps the old rule (a property of
    // the current class, by name).
    std::map<std::string, const ast::ClassDef*> classes;
    std::map<std::string, std::string> local_type;
    std::string ret_type;
    // Script inheritance (design 4.5). "native_base" is the native base of the root of the
    // script classes (empty: no native base). "native_virtuals" has the names of its virtual
    // methods. "super.M(args)" of such a name in a class with a script parent P becomes
    // "P__base_M(self, args)": the implementation that P sees.
    std::string native_base;
    std::set<std::string> native_virtuals;
    // The virtuals of the native interface bases (design 4.4): the name to the interface base.
    // "super.F()" of such a virtual calls the implementation of the interface base.
    std::map<std::string, std::string> interface_virtuals;
    // Set to true when a bare "this" becomes a handle (spp::handle_from_this). The translator
    // then gives a native-base shell the base std::enable_shared_from_this. May be null.
    bool* handle_this = nullptr;
    // String interpolation (design 7.3): "std" (the default) or "fmt". Set to true when a
    // string becomes a call of the format function. The translator then adds the include. May be null.
    std::string format_lib = "std";
    bool* uses_format = nullptr;
    // Module variables (design 5.3, 7.3): "Type name = init;" at file level. A bare name that is
    // not a local becomes "spp_mod()->name", an access to the module state block. The map has the
    // names and the script types. "in_module_init" is true in the initializer of a module
    // variable. There, only the variables in "module_earlier" are allowed: the loader runs the
    // initializers in declaration order, so a variable declared earlier has its value.
    // "in_module_hook" is true in a convert hook of a module variable: no module variable is
    // allowed there, because the new block is not complete.
    std::map<std::string, std::string> module_vars;
    bool in_module_init = false;
    std::set<std::string> module_earlier;
    bool in_module_hook = false;
};

// The context of a class: the fields and the script fns. A body or an initializer
// starts from a copy of it.
RCtx make_class_ctx(const ast::ClassDef& cd);
// Adds the parameters of a fn to the locals.
// It also records the script types of the parameters.
void add_params(RCtx& cx, const std::vector<ast::Param>& params);

// The number text in C++: "1.5" becomes "1.5f", "3L" becomes "int64_t{3}".
std::string rewrite_number(const std::string& n);

// Lowers the statements of a block. Each error goes to the diagnostics and the pass continues.
void lower_block(ast::BlockStmt& b, RCtx& cx, Diagnostics& diag);
// Lowers one expression. The pass can replace the node.
void lower_expr(ast::ExprP& e, RCtx& cx, Diagnostics& diag);

}  // namespace sppc
