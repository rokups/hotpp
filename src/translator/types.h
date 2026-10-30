// The type model: maps the class stubs to the C++ storage types (design 7.10).
// This file has no libclang and no lexy.
#pragma once

#include "ast.h"
#include "diag.h"

#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace hotc
{

enum class Kind
{
    Raw,
    Value,
    Handle
};

struct TypeRule
{
    Kind kind = Kind::Raw;
    std::string handle_tpl; // the handle template, "$T" is the class (Kind::Handle only)
    std::string owner; // the name of the stub that matched (empty for Raw)
    std::string intrusive_add, intrusive_release; // the member names of an intrusive handle, else empty
};

class TypeModel
{
public:
    // Adds the stubs of one list (a prelude or a script).
    // A later list overrides an earlier entry. A prelude gives no message.
    // A script that overrides a stub of a prelude gives a warning (design 7.10.3).
    // The map gives the line and the column for an error at the stub.
    // These are errors: two stubs of one name in one list, "value" together with "handle",
    // "default_handle" on a class other than "*", and any other attribute on "class *".
    enum class Source
    {
        Prelude,
        Script
    };
    void add_stubs(
        const std::vector<ast::Stub>& stubs, Diagnostics& diags, const LineMap& map, Source source = Source::Prelude);

    // The handle template of the stub "class *;" (empty: there is none).
    const std::string& default_handle() const { return default_handle_; }

    // What the headers say about a class. "known" is true for a class type that libclang found.
    // "plain" is true for a trivially copyable, standard-layout type.
    // "small" is true for a trivially copyable type of 16 bytes or less (design 7.10.5).
    struct ClassFacts
    {
        bool known = false, plain = false, small = false;
    };
    using ClassFn = std::function<ClassFacts(const std::string&)>;

    // Sets the callback for the facts of a class. The stub "class *;" needs it.
    void set_classes(ClassFn fn);

    // True when the facts of the type change its rule: the type is a class name and no stub
    // matches it. The facts decide rule 4 of 7.10.2 (plain data is a value) and the stub "class *;".
    bool wants_facts(const std::string& type) const;

    // Registers the script classes of the script (design 7.10.1). A script class is a handle,
    // unless it is a [value] class. A script class with a native base uses the handle of that
    // base (stub, base stub, pattern or "class *"). Else it uses std::shared_ptr<$T>.
    // A script class wins over a stub of the same name. Call it before lookup().
    void add_script_classes(const std::vector<ast::ClassDef>& classes);

    // True when the name is a [value] script class.
    bool is_script_value(const std::string& type) const;

    // The direct bases of a qualified C++ class name, in declaration order.
    // An unknown name gives an empty vector.
    using BaseFn = std::function<std::vector<std::string>(const std::string&)>;

    // Sets the callback for the bases. The default is no callback (no inheritance).
    void set_bases(BaseFn fn);

    // The rule of a C++ type, in this order:
    // 1) an exact stub,
    // 2) the first Handle stub on a base class (breadth first, nearest base wins),
    // 3) the longest pattern,
    // 4) a plain data class (trivially copyable, standard layout) is a Value,
    // 5) the stub "class *;": a class that is not plain gets its default handle,
    // 6) Raw.
    // Only the Handle kind is inherited. "owner" is the stub that matched.
    TypeRule lookup(const std::string& cpp_type) const;

    // True when the name is a script class of the script ([value] classes too).
    bool is_script_class(const std::string& type) const;

    // The names that the native headers declare in the global namespace (a function, a
    // variable, a type or a namespace). Only the names that the translator asked for are in
    // the set. A bare "enumerate" or "range" of such a name is not the script helper.
    void set_native_globals(std::set<std::string> names) { native_globals_ = std::move(names); }
    bool is_native_global(const std::string& name) const { return native_globals_.count(name) != 0; }

    // The C++ type of "T&", "T*&" and "const T&" when T is a script class that is a handle:
    // "storage(T)&" or "const storage(T)&". Else an empty string (the native types stay raw).
    // A ".cpp" script keeps raw pointers and references to a script class: the function
    // script_ref_storage() then gives an empty string.
    std::string script_ref_storage(const std::string& type) const;
    void set_raw_script_refs(bool raw) { raw_script_refs_ = raw; }

    // The C++ storage type of a declaration.
    std::string storage(const std::string& type) const;

    // The C++ type of a parameter: "const H&" for a handle, else storage().
    std::string param(const std::string& type) const;

    // The C++ type of a by-value parameter in the generation function of a script function
    // (design 7.10.5). "mutated", "rebound" and "stored" come from the analysis of the body (check.h).
    //  - A handle is "const H&", unless the body stores it or rebinds it: then it is "H".
    //  - A value is "const T&", unless the body mutates or stores it, or the type is small and
    //    trivially copyable (16 bytes or less): then it is "T".
    //  - Other types (builtin and raw) keep the form of param().
    // The slot adapter and the thunk keep the form of param(), so the call interface does not
    // change when a reload changes the body.
    std::string body_param(const std::string& type, bool mutated, bool rebound, bool stored) const;

    // The text of a new expression.
    std::string make(const std::string& type, const std::string& args) const;

    // How to build "new T(args)". The callee is the text before "(args)".
    // With wrap_new the result is "H(new T(args))", where H is the callee.
    struct Callee
    {
        std::string text;
        bool wrap_new = false;
    };
    Callee make_callee(const std::string& type) const;

    // True when the type is a plain handle type (not a pointer, a reference or a builtin).
    bool is_handle(const std::string& type) const;

private:
    TypeRule lookup_stub(const std::string& cpp_type) const;

    std::map<std::string, TypeRule> exact_;
    std::vector<std::pair<std::string, TypeRule>> patterns_; // the prefix and the rule
    BaseFn bases_;
    ClassFn classes_;
    std::string default_handle_;
    struct ScriptClass
    {
        std::string base;
        bool is_value = false;
    };
    std::map<std::string, ScriptClass> script_;
    std::set<std::string> native_globals_;
    bool raw_script_refs_ = false;
    std::set<std::string> prelude_names_; // the stubs that a prelude gave
};

} // namespace hotc
