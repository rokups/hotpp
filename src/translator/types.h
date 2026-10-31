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
    bool native = false; // the stub has the attribute [native]
    bool self = false; // an exact stub with [self]: the rule is for this type only
    std::string cpp_prelude; // the raw text of [cpp_prelude("...")] of the stub
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
    // A stub "class : Base;" is a rule for the subclasses of Base. It has no "value" and no
    // "default_handle". One "class : Base;" and one "struct *;" are allowed in a file.
    enum class Source
    {
        Prelude,
        Script
    };
    void add_stubs(
        const std::vector<ast::Stub>& stubs, Diagnostics& diags, const LineMap& map, Source source = Source::Prelude);

    // The namespace of the standard library names (module attribute std, std_ns.h). The text of a
    // stub with a plain "std::" matches the native text for this name only. The default is "std".
    void set_std_ns(std::string ns) { std_ns_ = std::move(ns); }

    // The handle template of the stub "class *;" (empty: there is none).
    const std::string& default_handle() const { return default_handle_; }

    // What the headers say about a class. "known" is true for a class type that libclang found.
    // "plain" is true for a trivially copyable, standard-layout type.
    // "small" is true for a trivially copyable type of 16 bytes or less (design 7.10.5).
    struct ClassFacts
    {
        bool known = false, plain = false, small = false;
        // The class derives (directly or not) from std::enable_shared_from_this<X>.
        bool shared_from_this = false;
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
    // 1) a script class (the walk of 3 gives its handle),
    // 2) an exact stub (with [self] or without),
    // 3) the bases, breadth first, nearest base first. At each base B:
    //    the stub "class : B;" wins (any kind, Raw too). Else the exact stub of B wins,
    //    when it is a Handle stub without [self]. An exact Raw stub of B does not stop the walk.
    // 4) the longest pattern,
    // 5) a plain data class (trivially copyable, standard layout) is a Value,
    // 6) the stub "class *;": a class that is not plain gets its default handle,
    // 7) Raw.
    // Only the Handle kind of an exact stub is inherited. "owner" is the stub that matched.
    TypeRule lookup(const std::string& cpp_type) const;

    // The C++ texts of cpp_prelude for a script class. The texts are raw (no "$T" is replaced).
    // The order: the text of "class *;" (a shell) or of "struct *;" (a struct), then each base
    // from the farthest to the nearest (the exact stub of the base when it has no [self], then
    // the stub "class : Base;"), then the text of the class itself. An empty text is not in the list.
    std::vector<std::string> preludes(const std::string& type) const;

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

    // True when the class has std::enable_shared_from_this<X> in its bases. The fact of libclang
    // for the class counts, and the fact of each base (the bases of a script class too).
    bool has_shared_from_this(const std::string& type) const;

    // The handle type T that a native parameter type text carries, else an empty string.
    // T is a handle class (a native stub or a script class). H is the native storage text of T,
    // for example "std::shared_ptr<Weapon>". The carriers of T are: "T*", "const T*", "T&",
    // "const T&", "H", "const H&" and "H&&". For an intrusive stub the storage text is "T*",
    // so "T*" is the handle carrier. The text goes through the canonical type spelling first.
    std::string carrier_of(const std::string& native_param_type) const;

    // The form of the carrier that carrier_of finds: 'p' for "T*" and "const T*", 'r' for "T&"
    // and "const T&", 'h' for "H", "const H&" and "H&&" (the "T*" of an intrusive stub is 'h').
    // The result is 0 when the text is not a carrier.
    char carrier_form(const std::string& native_param_type) const;

    // The handle type T of a native return type that is H by value ("std::shared_ptr<Weapon>"),
    // else an empty string. A pointer, a reference and the intrusive "T*" are not carriers of
    // a return: the lifetime of the result is not clear (task 33, phase 2).
    std::string return_carrier_of(const std::string& native_return_type) const;

private:
    std::string carrier_parse(const std::string& native_param_type, char& form) const;
    TypeRule lookup_stub(const std::string& cpp_type) const;

    // The direct bases of a type: the bases of a script class, else the base callback.
    std::vector<std::string> direct_bases(const std::string& name) const;
    // The rule that a base gives to its subclasses (empty owner: there is none).
    bool base_rule(const std::string& base, TypeRule& out) const;

    std::map<std::string, TypeRule> exact_;
    std::map<std::string, TypeRule> derived_; // the stubs "class : Base;", by the name of Base
    std::string class_prelude_, struct_prelude_; // the texts of "class *;" and "struct *;"
    std::vector<std::pair<std::string, TypeRule>> patterns_; // the prefix and the rule
    BaseFn bases_;
    ClassFn classes_;
    std::string default_handle_;
    std::string std_ns_ = "std"; // see set_std_ns()
    struct ScriptClass
    {
        std::string base;
        std::vector<std::string> interfaces;
        bool is_value = false;
        std::string cpp_prelude;
    };
    std::map<std::string, ScriptClass> script_;
    std::set<std::string> native_globals_;
    bool raw_script_refs_ = false;
    std::set<std::string> prelude_names_; // the stubs that a prelude gave
};

// A native virtual method as the matcher of an override sees it.
struct VirtualSig
{
    std::string name;
    std::vector<std::string> ptypes; // the native parameter type texts
    bool is_const = false;
};

// The result of match_native_virtual().
struct OverrideMatch
{
    enum class Kind
    {
        None, // no virtual matches
        Hit, // "hits" has one index
        Ambiguous, // "hits" has two or more indices
        ConstMismatch // the script marks "const" and every match is non-const; "hits" has the matches
    };
    Kind kind = Kind::None;
    bool relaxed = false; // the match needed the handle carriers
    std::vector<size_t> hits; // the indices in the list of virtuals
    // Kind::None only: the virtuals of the same name that the relaxed rule would match.
    std::vector<size_t> relaxed_candidates;
};

// The key of a type text for the match: the canonical spelling without spaces.
std::string override_key_norm(const std::string& type);

// Finds the native virtual that a script method overrides.
// "script_ptypes" are the parameter type texts of the script. The exact pass compares the
// canonical type texts. "F()" and "F() const" have the same key. "is_const" (a trailing
// "const" of the script) selects the const method. Without it the non-const method is the
// default, and a lone const method matches too.
// If the exact pass finds nothing and "relaxed" is true, the relaxed pass replaces each native
// parameter type that is a carrier of a handle type T (TypeModel::carrier_of) by T and compares
// again. The exact spelling always wins.
OverrideMatch match_native_virtual(const std::string& name, const std::vector<std::string>& script_ptypes,
    const std::vector<VirtualSig>& virtuals, const TypeModel& model, bool is_const, bool relaxed);

// True when the return type of a script override fits the return type of the native virtual:
// the canonical texts are equal, or "relaxed" is true and the native type is H by value
// (TypeModel::return_carrier_of) of the handle type that the script writes.
bool override_return_ok(
    const std::string& script_ret, const std::string& native_ret, const TypeModel& model, bool relaxed);

// The relaxed override match is on (task 33). A ".cpp" script does not use it.
constexpr bool relaxed_override_enabled()
{
    return true;
}

} // namespace hotc
