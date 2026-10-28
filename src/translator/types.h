// The type model: maps the class stubs to the C++ storage types (design 7.10).
// This file has no libclang and no lexy.
#pragma once

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "ast.h"
#include "diag.h"

namespace spc {

enum class Kind { Raw, Value, Handle };

struct TypeRule {
    Kind kind = Kind::Raw;
    std::string handle_tpl;  // the handle template, "$T" is the class (Kind::Handle only)
    std::string owner;       // the name of the stub that matched (empty for Raw)
    std::string intrusive_add, intrusive_release;  // the member names of an intrusive handle, else empty
};

class TypeModel {
public:
    // Adds the stubs of one list (a prelude or a script).
    // A later list overrides an earlier entry with no message.
    // The map gives the line and the column for an error at the stub.
    void add_stubs(const std::vector<ast::Stub>& stubs, Diagnostics& diags, const LineMap& map);

    // The direct bases of a qualified C++ class name, in declaration order.
    // An unknown name gives an empty vector.
    using BaseFn = std::function<std::vector<std::string>(const std::string&)>;

    // Sets the callback for the bases. The default is no callback (no inheritance).
    void set_bases(BaseFn fn);

    // The rule of a C++ type, in this order:
    // 1) an exact stub,
    // 2) the first Handle stub on a base class (breadth first, nearest base wins),
    // 3) the longest pattern,
    // 4) Raw.
    // Only the Handle kind is inherited. "owner" is the stub that matched.
    TypeRule lookup(const std::string& cpp_type) const;

    // The C++ storage type of a declaration.
    std::string storage(const std::string& type) const;

    // The C++ type of a parameter: "const H&" for a handle, else storage().
    std::string param(const std::string& type) const;

    // The C++ type of a "ref" or "out" parameter: storage() with "&". A handle has no "const".
    std::string ref_param(const std::string& type) const;

    // The text of a new expression.
    std::string make(const std::string& type, const std::string& args) const;

    // How to build "new T(args)". The callee is the text before "(args)".
    // With wrap_new the result is "H(new T(args))", where H is the callee.
    struct Callee {
        std::string text;
        bool wrap_new = false;
    };
    Callee make_callee(const std::string& type) const;

private:
    bool is_handle(const std::string& type) const;

    std::map<std::string, TypeRule> exact_;
    std::vector<std::pair<std::string, TypeRule>> patterns_;  // the prefix and the rule
    BaseFn bases_;
};

}  // namespace spc
