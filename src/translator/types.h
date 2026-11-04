// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

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
    std::string weak_tpl; // the template of a "[weak]" declaration (weak_handle), else empty
    std::string owner; // the name of the stub that matched (empty for Raw)
    std::string intrusive_add, intrusive_release; // the member names of an intrusive handle, else empty
    std::string factory_tpl; // the callee template of "new $T(args)" on a native class ([factory]), else empty
    bool native = false; // the stub has the attribute [native]
    bool self = false; // an exact stub with [self]: the rule is for this type only
    bool nonnull = false; // the stub has the attribute [nonnull]: a native call returns a non null value of the type
    std::string cpp_prelude; // the raw text of [cpp_prelude("...")] of the stub
    std::string cpp_overrides; // the raw text of [cpp_overrides("...")] of the stub
};

// True for the base names of a script exception type: "ScriptError", "hot.ScriptError" and
// "hot::ScriptError".
bool is_exception_base(const std::string& name);

class TypeModel
{
public:
    // Adds the stubs of one list (a prelude or a script). The lists come in order: the preludes,
    // then the script. A stub with the key of a stub of an earlier list merges into that rule:
    // each attribute that the later stub states replaces the earlier value. "value", "handle" and
    // "intrusive" are one storage choice, and a later one replaces the earlier one. The texts of
    // "cpp_prelude" stack (the later text comes after the earlier text), and the names of
    // "cpp_overrides" add up. An empty string clears a text attribute. The flags "native", "self" and
    // "checked_index" stay set. The map gives the line and the column for an error at the stub.
    // These are errors: two stubs of one key in one list, "value" together with "handle",
    // "default_handle" on a class other than "*", any other attribute on "class *", and a merged
    // rule that breaks one of these rules.
    // A stub "class : Base;" is a rule for the subclasses of Base. It has no "value" and no
    // "default_handle". One "class : Base;" and one "struct *;" are allowed in a file.
    enum class Source
    {
        Prelude,
        Script
    };
    void add_stubs(
        const std::vector<ast::Stub>& stubs, Diagnostics& diags, const LineMap& map, Source source = Source::Prelude);

    // The key of a stub: "struct *", ": Base" for "class : Base;", else the name.
    static std::string stub_key(const ast::Stub& s);
    // The merged stubs, in the order of the first stub of each key. A merged stub has the position
    // of the last stub of its key.
    std::vector<const ast::Stub*> merged_stubs() const;

    // The namespace of the standard library names (module attribute std, std_ns.h). The text of a
    // stub with a plain "std::" matches the native text for this name only. The default is "std".
    void set_std_ns(std::string ns) { std_ns_ = std::move(ns); }
    const std::string& std_ns() const { return std_ns_; }

    // The standard-like library namespaces (the module attribute "lib"). A handle template
    // "L::shared_ptr<$T>" of such a namespace L works like the std one: "L::weak_ptr" is its weak
    // handle and "L::make_shared" makes an object.
    void set_libs(std::vector<std::string> libs) { libs_ = std::move(libs); }

    // True when a native call that returns the type gives a non null value: the stub of the type has
    // [nonnull], or the stub "class *;" has it (the library default). The nullability analyzer reads it.
    // The type is a C++ type text. A "const", a "&", a "*" or a handle template around it does not count.
    bool returns_nonnull(const std::string& cpp_type) const;

    // The handle template of the stub "class *;" (empty: there is none).
    const std::string& default_handle() const { return default_handle_; }

    // What the headers say about a class. "known" is true for a class type that libclang found.
    // "plain" is true for a trivially copyable, standard-layout type.
    struct ClassFacts
    {
        bool known = false, plain = false;
        // The type is an enumeration (a native enum). "known" is false for it.
        bool is_enum = false;
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
    // unless it is a struct. A script class with a native base uses the handle of that
    // base (stub, base stub, pattern or "class *"). Else it uses std::shared_ptr<$T>.
    // A script class wins over a stub of the same name. Call it before lookup().
    void add_script_classes(const std::vector<ast::ClassDef>& classes);

    // Registers the script interfaces (design 41b). An interface name is a known type, but it
    // is not a class and it has no handle rule. Its storage is "hot::Iface<I__iface>" (D4).
    void add_script_interfaces(const std::vector<std::string>& names);
    bool is_script_interface(const std::string& type) const;

    // The C++ type of an interface type: "I" gives "hot::Iface<I__iface>". "const I", "I&" and
    // "const I&" keep the "const" and the "&". Empty when the type is not an interface type.
    std::string iface_storage(const std::string& type) const;

    // True when the name is a struct.
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

    // The names of the native virtuals that the cpp_prelude texts of a script class override.
    // The stubs are the same as for preludes(). The shell gets no slot for these virtuals.
    std::set<std::string> host_overrides(const std::string& type) const;

    // True when the name is a script class of the script (structs too).
    bool is_script_class(const std::string& type) const;

    // The first class in the parent chain of a script class that is not a script class: the native
    // base. An empty string: the chain ends in a script class with no base, or the type is not
    // a script class, or the type is a struct (also an exception struct).
    std::string script_native_base(const std::string& type) const;

    // True when the name is a struct of the script that is a script exception type: its base is
    // ScriptError or another exception struct.
    bool is_exception_struct(const std::string& type) const;

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

    // Where a type text is used. It selects the form of the C++ type.
    enum class StorageUse
    {
        Field,  // a field or a module variable
        Param,  // a plain parameter: a value is "hot::In<T>", a handle is "const H&"
        LambdaParam, // a lambda parameter: a value goes by value, a handle by "const H&"
        Return, // a return type
        Local   // a local variable, a foreach element, a cast or a template argument
    };

    // The one mapper from a script type text to the C++ type text. Every pass calls it. The text is
    // the canonical script type (a "T[N]" is an array template already). The rules:
    //  - "$C" is the handle form of the rule of C (default "std::shared_ptr<C>"). "$S" on a struct
    //    or a value type is "std::shared_ptr<S>".
    //  - A bare script class "C" is the box "hot::Value<C>" (see value_storage()). Only "$C" is the
    //    handle. A class with a native base has no box: the checker reports it.
    //  - "T*" stays "T*". A field "T*" of a native class with a weak source is "hot::CheckedPtr<T>".
    //  - "T?" is "std::optional<M>", where M is the mapping of T.
    //  - "T[]" is "std::span<M>".
    //  - The builtin types and the other types are as storage() and param() give them.
    // The mark "?" needs a value type. A pointer, a "$" handle, a script class and an interface are
    // nullable: "T?" on them is an error. The error text goes to "error" (if not null) and the
    // result is an empty string. A script is ".cpp" or ".hot" as set_raw_script_refs() tells.
    std::string cpp_storage(const std::string& script_type, StorageUse use, std::string* error = nullptr) const;

    // The rules for one class or one value type below cpp_storage(). A pass does not call them.
    // The C++ storage type of a declaration. A script class that is a template argument gets
    // the handle storage, for example "std::vector<Child>" gives "std::vector<H>" with
    // H = storage("Child"). The rewrite is recursive. A smart pointer template keeps its argument.
    std::string storage(const std::string& type) const;

    // The type with each script class in a template argument rewritten to its handle storage.
    std::string targ_storage(const std::string& type) const;

    // The C++ storage type of a field or module variable of type "T*" or "const T*" (one star),
    // where T is a native class: "hot::CheckedPtr<T>" or "hot::CheckedPtr<const T>". T is not a
    // builtin, an enumeration, a script class or an intrusive stub (that "T*" is the handle).
    // Any other type gives an empty string.
    std::string checked_ptr_storage(const std::string& type) const;

    // True when the type is "T*" or "const T*" (one star) and T is a builtin type or a native
    // enumeration. Nothing owns such a pointee, so a field cannot point to it.
    bool is_value_pointer(const std::string& type) const;

    // The C++ storage type of a "[weak] T" declaration (task 38): "std::weak_ptr<T>" with the
    // namespace of the shared pointer template. The handle of T must be a std::shared_ptr. Any
    // other type (a value, an intrusive handle, a raw type) gives an empty string.
    std::string weak_storage(const std::string& type) const;

    // The C++ storage type of a "C" (a bare script class) declaration: "hot::Value<E>". E is the element type of
    // the handle of T (the shell of the script class). T must be a script class with no native base
    // and a std::shared_ptr handle. Any other type gives an empty string.
    std::string value_storage(const std::string& type) const;

    // The handle templates of the stubs that have the plain form "Name<$T>" and are not
    // "std::shared_ptr". The result is the qualified "Name" (the std placeholder expanded is not
    // done). The module header specializes hot::is_handle for each one.
    std::vector<std::string> custom_handle_templates() const;

    // A stub class is a weak source when its exact stub has a "handle" (not intrusive) with a weak
    // type: the "weak_handle" template, or the weak_ptr of a shared_ptr handle.
    struct StubWeakSource
    {
        std::string name;  // the class of the exact stub
        std::string bases; // the base class text of the traits: "U" is the class type
    };
    // The weak sources of the exact stubs. The module header writes one weak_ref_traits for each.
    std::vector<StubWeakSource> stub_weak_sources() const;

    // The C++ type of a plain parameter (a read only input), by one rule (design: "Parameter defaults"):
    //  - A handle is "const H&".
    //  - A builtin type, a pointer and a reference keep their form.
    //  - Any other value is "hot::In<T>". The C++ trait picks "T" or "const T&" (hot/runtime/in.h).
    std::string param(const std::string& type) const;

    // The C++ type of a parameter in the generation function of a script function (design 7.10.5).
    // "mutated", "rebound" and "stored" come from the analysis of the body (check.h).
    //  - A handle is "const H&", unless the body stores it or rebinds it: then it is "H".
    //  - A value is the form of param(), unless the body mutates it: then it is "T", a copy.
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

    // True when "derived" has "base" in its bases, directly or not. The names are equal: false.
    // A script class uses its script bases and its interfaces. A native class uses the base
    // callback (set_bases). An unknown name has no bases.
    bool derives_from(const std::string& derived, const std::string& base) const;

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
    // The error of the marks "?" in a type text, else an empty string.
    std::string marks_error(const std::string& type) const;
    // True when the type (a template argument, no "&") is nullable: a handle, a pointer, an interface.
    bool is_nullable(const std::string& type) const;
    std::string map_marked(const std::string& type, StorageUse use) const;
    std::string map_plain(const std::string& type, StorageUse use) const;
    std::string param_form(const std::string& type, bool in) const;
    std::string carrier_parse(const std::string& native_param_type, char& form) const;
    TypeRule lookup_stub(const std::string& cpp_type) const;

    // The direct bases of a type: the bases of a script class, else the base callback.
    std::vector<std::string> direct_bases(const std::string& name) const;
    // The rule that a base gives to its subclasses (empty owner: there is none).
    bool base_rule(const std::string& base, TypeRule& out) const;

    std::map<std::string, TypeRule> exact_;
    std::map<std::string, TypeRule> derived_; // the stubs "class : Base;", by the name of Base
    std::string class_prelude_, struct_prelude_; // the texts of "class *;" and "struct *;"
    std::string class_overrides_; // the cpp_overrides text of "class *;"
    std::vector<std::pair<std::string, TypeRule>> patterns_; // the prefix and the rule
    BaseFn bases_;
    ClassFn classes_;
    std::string default_handle_;
    bool default_nonnull_ = false; // the stub "class *;" has [nonnull]
    std::string default_weak_; // the weak_handle of "class *"
    std::string std_ns_ = "std"; // see set_std_ns()
    std::vector<std::string> libs_; // see set_libs()
    // The namespace prefix of a shared pointer template: "\x01std::", "std::", "L::" or empty.
    std::string shared_prefix(const std::string& tpl) const;
    struct ScriptClass
    {
        std::string base;
        std::vector<std::string> interfaces;
        bool is_value = false;
        std::string cpp_prelude;
        std::string cpp_overrides;
    };
    std::map<std::string, ScriptClass> script_;
    std::set<std::string> script_ifaces_; // the names of the script interfaces
    std::set<std::string> native_globals_;
    bool raw_script_refs_ = false;
    std::map<std::string, ast::Stub> merged_; // the merged stub of each key
    std::vector<std::string> merged_order_; // the keys in the order of their first stub
};

// The message for a "[weak] T" declaration when weak_storage(T) is empty (task 38).
std::string weak_handle_error(const std::string& script_type);

// The message for "[weak] I" when "I" is a script interface. The attribute is "weak".
std::string interface_attribute_error(const std::string& attribute, const std::string& script_type);

// The error texts of the values of a script class (the box "hot::Value<C>") and of "null".
inline constexpr const char* kNativeBaseValueError =
    "a script class with a native base cannot be a value; write new $C(...)";
// The error of a shared pointer that comes from a raw pointer ("$T" rules of the type design).
inline constexpr const char* kRawToSharedError =
    "cannot create a shared pointer from a raw pointer; use new $T(...)";
inline constexpr const char* kNullValueError = "a value cannot be null; write $C w";
inline constexpr const char* kNullTypeError = "null has no type; write the type";

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
        Ambiguous // "hits" has two or more indices
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
// canonical type texts. "F()" and "F() const" have the same key. The script cannot write "const"
// on a method, so the non-const method is the default, and a lone const method matches too.
// If the exact pass finds nothing and "relaxed" is true, the relaxed pass replaces each native
// parameter type that is a carrier of a handle type T (TypeModel::carrier_of) by T and compares
// again. The exact spelling always wins.
OverrideMatch match_native_virtual(const std::string& name, const std::vector<std::string>& script_ptypes,
    const std::vector<VirtualSig>& virtuals, const TypeModel& model, bool relaxed);

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
