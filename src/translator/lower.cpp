// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The lowering pass. See lower.h.
#include "lower.h"

#include "builtin_types.h"
#include "emit.h"
#include "operator_names.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <optional>
#include <set>
#include <utility>

namespace hotc
{

std::string rewrite_number(const std::string& n)
{
    if (n.size() > 1 && n[0] == '0' && (n[1] == 'x' || n[1] == 'X'))
        return n;
    bool fp = n.find_first_of(".eE") != std::string::npos;
    char last = n.back();
    if (fp && (last == 'd' || last == 'D'))
        return n.substr(0, n.size() - 1);
    if (fp && (last == 'f' || last == 'F'))
        return n;
    if (fp)
        return n + "f";
    if (last == 'L')
        return "int64_t{" + n.substr(0, n.size() - 1) + "}";
    return n;
}

RCtx make_class_ctx(const ast::ClassDef& cd)
{
    RCtx cx;
    cx.cls = &cd;
    for (const ast::Field& f : cd.fields)
    {
        cx.field_type[f.name] = f.type;
        if (f.weak)
            cx.weak_fields.insert(f.name);
        if (f.value)
            cx.value_fields.insert(f.name);
    }
    for (const ast::Method& m : cd.methods)
        if (!m.overrides_virtual)
            cx.fns.insert(m.name);
    for (const ast::Property& p : cd.props)
        cx.props[p.name] = {p.has_get, p.has_set};
    return cx;
}

// The type of the object that a reference type names: "const T&" and "T&" give "T". "T*&" gives
// "T", because a reference to a script class pointer is a reference to the handle. Other types stay.
static std::string object_type(const std::string& type)
{
    size_t e = type.find_last_not_of(" \t");
    if (e == std::string::npos || type[e] != '&' || (e > 0 && type[e - 1] == '&'))
        return type;
    std::string t = type.substr(0, e);
    if (t.compare(0, 6, "const ") == 0)
        t = t.substr(6);
    while (!t.empty() && (t.back() == ' ' || t.back() == '\t'))
        t.pop_back();
    if (!t.empty() && t.back() == '*')
        t.pop_back();
    return t;
}

void add_params(RCtx& cx, const std::vector<ast::Param>& params)
{
    for (const ast::Param& p : params)
    {
        cx.locals.insert(p.name);
        if (p.weak)
            cx.weak_locals.insert(p.name);
        else
            cx.weak_locals.erase(p.name);
        if (p.value)
            cx.value_locals.insert(p.name);
        else
            cx.value_locals.erase(p.name);
        const std::string t = object_type(p.script_type.empty() ? p.type : p.script_type);
        if (t.empty())
            cx.local_type.erase(p.name);
        else
            cx.local_type[p.name] = t;
    }
}

namespace
{

using namespace ast;

// A name with one part and no template arguments. Gives the part or null.
const std::string* simple_name(const Expr& e)
{
    const auto* n = std::get_if<NameExpr>(&e.v);
    if (!n || n->parts.size() != 1 || !n->template_args.empty())
        return nullptr;
    return &n->parts[0];
}

// A catch clause for an exception type of the runtime (include/hot/runtime/errors.h): the type
// text "NullReference", "hot.IndexOutOfRange" or "const hot::ScriptError &" becomes
// "const hot::X&". Another type text becomes a const reference too, so the handler never slices an
// exception. A type with a "&" or a "*" stays as it is.
void normalize_runtime_catch_type(std::string& type)
{
    std::string t = type;
    auto trim = [&t]()
    {
        const size_t b = t.find_first_not_of(" \t");
        const size_t e = t.find_last_not_of(" \t");
        t = b == std::string::npos ? std::string() : t.substr(b, e - b + 1);
    };
    trim();
    if (t.compare(0, 6, "const ") == 0)
    {
        t.erase(0, 6);
        trim();
    }
    if (!t.empty() && t.back() == '&')
    {
        t.pop_back();
        trim();
    }
    for (const char* prefix : {"hot::", "hot."})
    {
        const size_t n = std::char_traits<char>::length(prefix);
        if (t.compare(0, n, prefix) == 0)
        {
            t.erase(0, n);
            break;
        }
    }
    if (t == "NullReference" || t == "IndexOutOfRange" || t == "ScriptError" || t == "TaskCancelled"
        || t == "TaskReloadCancelled" || t == "ArithmeticError")
        type = "const hot::" + t + "&";
    else if (!type.empty() && type.find_first_of("&*") == std::string::npos)
        type = "const " + type + "&";
}

// The member of "this". Gives the node or null.
const MemberExpr* this_member(const Expr& e)
{
    const auto* m = std::get_if<MemberExpr>(&e.v);
    if (!m || !std::holds_alternative<ThisExpr>(m->obj->v))
        return nullptr;
    return m;
}

struct Lowerer
{
    RCtx& cx;
    Diagnostics& diag;

    // The state of a finally block. The body of a finally block is a C++ lambda, so a jump
    // out of it is an error. A "return" in it is always an error. A "break" or "continue"
    // is an error when its target is outside the finally block.
    struct FinallyState
    {
        bool inside = false; // true in the body of a finally block
        int loops = 0; // the loops in the finally body around the current statement
        int switches = 0; // the switches in the finally body around the current statement
    };
    FinallyState fin;

    // The collection literals that have a declared type. Only these are allowed.
    std::set<const Expr*> typed_collections;
    // The number of the next string switch. It makes the name of the local that holds the value.
    int string_switches = 0;

    void error(uint32_t pos, const std::string& msg)
    {
        if (cx.map)
            diag.add(pos, *cx.map, msg);
        else
            diag.add_line(0, msg);
    }

    ExprP raw(std::string text, uint32_t pos) { return make_expr(RawExpr{std::move(text), pos}); }
    // Raw text "prefix + name" where the name comes from the script name at name_pos.
    ExprP raw_named(const std::string& prefix, const std::string& name, uint32_t pos, uint32_t name_pos)
    {
        return make_expr(RawExpr{prefix + name, pos, name_pos, static_cast<uint32_t>(prefix.size())});
    }

    // A property of the class. "obj" is null for "this.P" and a bare "P". Else it is the
    // object of "obj.P": another object of the class, before the lowering.
    struct PropRef
    {
        std::string name;
        ExprP* obj = nullptr;
        const ClassDef* cls = nullptr; // the class that has the property
        const ClassDef* via = nullptr; // the static class of "obj", or null for "this"
    };

    // ---- type information (see RCtx)
    const ClassDef* class_named(const std::string& t) const
    {
        auto it = cx.classes.find(t.compare(0, 1, "$") == 0 ? t.substr(1) : t);
        return it == cx.classes.end() ? nullptr : it->second;
    }

    // True when the script type text is "T?": an optional. A "const" prefix and a "&" suffix do not count.
    static bool is_optional(const std::string& type)
    {
        std::string t = type;
        while (!t.empty() && (t.back() == '&' || t.back() == ' ' || t.back() == '\t'))
            t.pop_back();
        return !t.empty() && t.back() == '?';
    }

    // "$C" names the handle of a script class, an interface or a native handle class. A bare "C" is
    // the same handle in this step, so the lowering drops the mark and goes on with "C".
    void drop_handle_mark(std::string& type) const
    {
        if (type.compare(0, 1, "$") != 0 || !cx.types)
            return;
        const std::string base = type.substr(1);
        if (cx.types->is_handle(base) || cx.types->is_script_interface(base))
            type = base;
    }

    // The script parent of a class, or null (design 4.5).
    const ClassDef* parent_of(const ClassDef& c) const
    {
        return c.base.empty() || c.base == c.name ? nullptr : class_named(c.base);
    }

    // A property of the class or of a script parent. The nearest class wins. "owner" gets the
    // class that declares the property (it has the accessor functions).
    const Property* find_prop(const ClassDef& c, const std::string& n, const ClassDef** owner = nullptr) const
    {
        int depth = 0;
        for (const ClassDef* d = &c; d && depth < 64; d = parent_of(*d), ++depth)
            for (const Property& p : d->props)
                if (p.name == n)
                {
                    if (owner)
                        *owner = d;
                    return &p;
                }
        return nullptr;
    }

    // A field of the class or of a script parent.
    const Field* find_field(const ClassDef& c, const std::string& n, const ClassDef** owner = nullptr) const
    {
        int depth = 0;
        for (const ClassDef* d = &c; d && depth < 64; d = parent_of(*d), ++depth)
            for (const Field& f : d->fields)
                if (f.name == n)
                {
                    if (owner)
                        *owner = d;
                    return &f;
                }
        return nullptr;
    }

    // ---- access words (design: public, protected, private)
    // True when the code that the pass lowers can use a member with the access "a" that the
    // class "owner" declares. A private member is for the class itself. A protected member is
    // also for the script subclasses, but only through an object of the subclass or of a
    // subclass of it ("via" is the static class of the object; null is "this"). A lambda in the
    // class has the same class. The pass does not check a native member.
    bool can_access(Access a, const ClassDef& owner, const ClassDef* via = nullptr) const
    {
        if (a == Access::Public)
            return true;
        if (!cx.cls)
            return false;
        if (cx.cls == &owner || (!owner.name.empty() && cx.cls->name == owner.name))
            return true;
        if (a == Access::Protected)
        {
            int depth = 0;
            for (const ClassDef* p = parent_of(*cx.cls); p && depth < 64; p = parent_of(*p), ++depth)
                if (p == &owner || p->name == owner.name)
                    return !via || is_same_or_sub(*via, *cx.cls);
        }
        return false;
    }

    // True when "c" is the class "base" or has "base" as a script ancestor.
    bool is_same_or_sub(const ClassDef& c, const ClassDef& base) const
    {
        int depth = 0;
        for (const ClassDef* p = &c; p && depth < 64; p = parent_of(*p), ++depth)
            if (p == &base || p->name == base.name)
                return true;
        return false;
    }

    // Reports a use of a private or protected member from a place that cannot use it.
    // "what" is the text before "is": "'name'", or "the 'set' accessor of 'name'".
    void check_access(
        Access a, const ClassDef& owner, const std::string& what, uint32_t pos, const ClassDef* via = nullptr)
    {
        if (can_access(a, owner, via))
            return;
        std::string msg = what + " is " + (a == Access::Private ? "private" : "protected") + " in '" + owner.name + "'";
        // The class may use the member, but not through this object.
        if (a == Access::Protected && can_access(a, owner))
            msg += " (use it through '" + cx.cls->name + "' or a subclass)";
        error(pos, msg);
    }
    void check_member(
        Access a, const ClassDef& owner, const std::string& name, uint32_t pos, const ClassDef* via = nullptr)
    {
        check_access(a, owner, "'" + name + "'", pos, via);
    }

    // A script function of the class or of a script parent: not an override of a native
    // virtual and not a hook. The nearest class wins. "owner" gets the class of the function.
    const Method* find_fn(const ClassDef& c, const std::string& n, const ClassDef** owner = nullptr) const
    {
        if (n == "onReload" || n.rfind("convert_", 0) == 0)
            return nullptr;
        int depth = 0;
        for (const ClassDef* d = &c; d && depth < 64; d = parent_of(*d), ++depth)
            for (const Method& m : d->methods)
                if (m.name == n && !m.overrides_virtual)
                {
                    if (owner)
                        *owner = d;
                    return &m;
                }
        return nullptr;
    }

    // A static method of the class or of a script parent. The nearest class wins. "owner" gets
    // the class of the method.
    const Method* find_static(const ClassDef& c, const std::string& n, const ClassDef** owner = nullptr) const
    {
        int depth = 0;
        for (const ClassDef* d = &c; d && depth < 64; d = parent_of(*d), ++depth)
            for (const Method& m : d->statics)
                if (m.name == n)
                {
                    if (owner)
                        *owner = d;
                    return &m;
                }
        return nullptr;
    }

    // A function that a bare name can call: a static method of the class chain, then a free
    // function. A local and an instance method of the class hide it. "owner" is null for a free function.
    const Method* bare_function(const std::string& n, const ClassDef** owner = nullptr) const
    {
        if (cx.locals.count(n) || cx.fns.count(n))
            return nullptr;
        if (owner)
            *owner = nullptr;
        // The class of a free function is the synthetic class of the free functions: skip it.
        if (cx.cls && !cx.in_free_fn)
            if (const Method* m = find_static(*cx.cls, n, owner))
                return m;
        // A field, a property and a member of the native base hide a free function.
        if (is_this_member(n))
            return nullptr;
        if (cx.free_fns)
            for (const Method& m : *cx.free_fns)
                if (m.name == n)
                    return &m;
        return nullptr;
    }

    // The static method that the name "C.F" calls: "C" is a script class that no local, field or
    // module variable hides.
    const Method* scope_static(const NameExpr& n, const ClassDef** owner = nullptr) const
    {
        if (n.parts.size() != 2 || !n.template_args.empty() || cx.locals.count(n.parts[0]) || is_module_var(n.parts[0])
            || is_this_member(n.parts[0]))
            return nullptr;
        const ClassDef* d = class_named(n.parts[0]);
        return d && !d->name.empty() && n.parts[0] == d->name ? find_static(*d, n.parts[1], owner) : nullptr;
    }

    static std::string static_hint(const ClassDef& owner, const std::string& n)
    {
        return "'" + n + "' is a static method; call it as '" + owner.name + "." + n + "(...)'";
    }

    // The error for the use of "this" where no object exists.
    void no_this_error(uint32_t pos)
    {
        if (cx.in_static)
            error(pos, cx.in_free_fn ? "'this' is not allowed in a free function"
                                     : "'this' is not allowed in a static method");
        else
            error(pos, "'this' is not allowed in a field initializer");
    }

    // The error for a bare member of the class in a static method.
    void needs_object_error(const std::string& n, uint32_t pos)
    {
        error(pos, "'" + n + "' needs an object: '" + cx.static_name + "' is a static method");
    }

    // The C++ name of a static method ("C__s_F") or of a free function ("hot_fn_F").
    static std::string static_symbol(const ClassDef* owner, const std::string& name)
    {
        return static_fn_symbol(owner ? owner->name : std::string(), name);
    }

    // A call of a static method or of a free function. The callee becomes the generated name. The
    // call has no "__self" argument. The arguments get the checks of a method call.
    void static_call(CallExpr& c, const Method& fn, const ClassDef* owner, const std::string& name, uint32_t pos)
    {
        // These errors come first: the symbol of the callee can be wrong in these places.
        if (cx.in_module_init)
            error(pos,
                "a module variable initializer cannot call the function '" + name
                    + "': the module state is not complete yet");
        else if (cx.in_module_hook)
            error(pos,
                "a convert hook cannot call the function '" + name + "': the new module state is not complete yet");
        else if (cx.in_field_init)
            error(pos,
                "a field initializer cannot call the function '" + name
                    + "': the prototype of the function comes after the class");
        else if (cx.in_default)
            error(pos,
                "a default argument cannot call the function '" + name
                    + "': the prototype of the function comes after the declaration");
        if (owner)
            check_member(fn.access, *owner, name, pos);
        c.callee = raw_named("", static_symbol(owner, name), pos, 0);
        coerce_args(c, fn.params, 0);
        exprs(c.args);
    }

    // The C++ name for a call of the script function "m" of "owner". A virtual function goes
    // through the dispatcher of its slot (design 7.6). Else the call is direct.
    static std::string fn_symbol(const ClassDef& owner, const Method& m)
    {
        return m.vslot_owner.empty() ? owner.name + "__" + m.name : m.vslot_owner + "__" + m.name + "_virt";
    }

    static const std::string& type_text(const Field& f) { return f.script_type.empty() ? f.type : f.script_type; }

    // The type of an element of a standard container type (task 48): a script class that is a
    // handle, or a container type (it has an element again). Else empty: the element is a native
    // value and the C++ code finds its type.
    // "sequence_only": a map gives no element, because a loop over a map gives pairs.
    std::string element_type(const std::string& container, bool sequence_only = false) const
    {
        const std::string t = container_element_type(container, sequence_only);
        if (t.empty())
            return {};
        const ClassDef* d = class_named(t);
        return (d && !d->is_value) || iface_named(t) || !container_element_type(t).empty() ? t : std::string();
    }

    // ---- script interfaces (design 41b)
    // The script interface that the type text names ("I", "const I&"), or null.
    const InterfaceDef* iface_named(const std::string& t) const
    {
        if (cx.iface_defs.empty())
            return nullptr;
        std::string b = t;
        while (!b.empty() && (b.back() == '&' || b.back() == ' ' || b.back() == '\t'))
            b.pop_back();
        if (b.compare(0, 6, "const ") == 0)
            b.erase(0, 6);
        if (b.compare(0, 1, "$") == 0)
            b.erase(0, 1);
        const auto it = cx.iface_defs.find(b);
        return it == cx.iface_defs.end() ? nullptr : it->second;
    }

    static const Method* iface_method(const InterfaceDef& d, const std::string& name)
    {
        for (const Method& m : d.methods)
            if (m.name == name)
                return &m;
        return nullptr;
    }

    static std::string ret_text(const Method& m) { return m.script_ret.empty() ? m.ret : m.script_ret; }

    // True when the class "c" or a script parent of it lists the interface.
    bool class_lists_iface(const ClassDef& c, const std::string& iface) const
    {
        int depth = 0;
        for (const ClassDef* d = &c; d && depth < 64; d = parent_of(*d), ++depth)
            if (std::find(d->script_ifaces.begin(), d->script_ifaces.end(), iface) != d->script_ifaces.end())
                return true;
        return false;
    }

    // The type of "a?.b" when the member "b" has the type "t". A handle stays a handle (null when "a"
    // is null). Any other type becomes an optional in the C++ code, so it is not known here.
    std::string safe_result_type(const std::string& t) const
    {
        if (iface_named(t))
            return t;
        const ClassDef* d = class_named(t);
        return d && !d->is_value ? t : std::string();
    }

    // True when the type text names the handle of a script class ("$C").
    bool is_class_handle_text(const std::string& t) const
    {
        const ClassDef* d = t.compare(0, 1, "$") == 0 ? class_named(t) : nullptr;
        return d && !d->is_value;
    }

    // The operator method that the use "e" calls on a value of a script class or struct (operator_names.h),
    // or null. A handle operand has no operator, and "x != y" uses the method of "x == y".
    const Method* op_use(const Expr& e) const
    {
        std::optional<std::string> ident;
        std::string lt;
        if (auto* b = std::get_if<BinaryExpr>(&e.v))
        {
            if (!b->lhs || !b->rhs)
                return nullptr;
            ident = b->op == "!=" ? std::optional<std::string>("op_eq") : op_ident(b->op, 1);
            if (ident && *ident != "op_index")
                lt = type_of(*b->lhs);
        }
        else if (auto* u = std::get_if<UnaryExpr>(&e.v))
        {
            if (u->postfix || !u->operand)
                return nullptr;
            ident = op_ident(u->op, 0);
            if (ident)
                lt = type_of(*u->operand);
        }
        else if (auto* ix = std::get_if<IndexExpr>(&e.v))
        {
            if (!ix->obj)
                return nullptr;
            ident = "op_index";
            lt = type_of(*ix->obj);
        }
        if (!ident || lt.empty() || lt[0] == '$')
            return nullptr;
        const ClassDef* d = class_named(lt);
        return d ? find_fn(*d, *ident) : nullptr;
    }

    // An operator on a handle of a script class has no meaning: the operators are for values. Reports
    // the error and returns true. "==" and "!=" compare the identity of two handles, so they are fine.
    bool op_on_handle(const Expr& operand, const std::string& sym, size_t arity, uint32_t pos)
    {
        if ((sym == "==" || sym == "!=") && arity == 1)
            return false;
        if (!op_ident(sym, arity))
            return false;
        const std::string t = type_of(operand);
        if (!is_class_handle_text(t))
        {
            // "T?" is a C++ optional: the C++ operator of the optional is not the operator of the script type.
            if (!is_optional(t))
                return false;
            std::string base = t;
            while (!base.empty() && (base.back() == '&' || base.back() == ' ' || base.back() == '?'))
                base.pop_back();
            const ClassDef* d = class_named(base);
            if (!d || !find_fn(*d, *op_ident(sym, arity)))
                return false;
            error(pos, "operator '" + sym + "' needs a value, not an optional");
            return true;
        }
        error(pos, "operator '" + sym + "' needs a value, not a handle");
        return true;
    }

    // The text of a parameter type as a type name: no "const", no "&".
    static std::string param_class_text(const Param& p)
    {
        std::string t = p.script_type.empty() ? p.type : p.script_type;
        if (t.compare(0, 6, "const ") == 0)
            t.erase(0, 6);
        while (!t.empty() && (t.back() == '&' || t.back() == ' '))
            t.pop_back();
        return t;
    }

    // Why the right operand "rhs" of the operator method "m" is not a value: "null", "a handle" or
    // "an optional". Empty: the operand can be a value. A class of an unrelated type is "a value of another class".
    std::string op_rhs_problem(const Method& m, const Expr& rhs) const
    {
        if (std::holds_alternative<NullExpr>(rhs.v))
            return "null";
        const std::string t = type_of(rhs);
        if (is_class_handle_text(t))
            return "a handle";
        if (is_optional(t))
            return "an optional";
        const ClassDef* c = class_named(t);
        if (c && !m.params.empty())
        {
            const std::string want = param_class_text(m.params[0]);
            if (class_named(want))
            {
                int depth = 0;
                for (const ClassDef* d = c; d && depth < 64; d = class_named(d->base), ++depth)
                    if (d->name == want)
                        return {};
                return "a value of another class";
            }
        }
        return {};
    }

    // The operator uses "a + b", "a == b" and the others on a value of a script class or struct: a
    // right operand that is null, a handle or an optional is an error. Reports it and returns true.
    // "==" and "!=" have the message of the box compare, so they are not here.
    bool op_rhs_error(const Expr& e, const BinaryExpr& b, uint32_t pos)
    {
        if (b.op == "==" || b.op == "!=" || !b.rhs)
            return false;
        const Method* m = op_use(e);
        if (!m)
            return false;
        const std::string why = op_rhs_problem(*m, *b.rhs);
        if (why.empty())
            return false;
        if (why == "a value of another class")
            error(pos, "operator '" + b.op + "' of the type does not take a value of this class");
        else
            error(pos, "operator '" + b.op + "' needs a value, not " + why);
        return true;
    }

    // "a += b": the compound operator is generated from the operator "+" of the class of "a", when
    // "+" returns the class and the class itself declares it. Any other use is an error. Reports it
    // and returns true.
    bool op_compound_error(const BinaryExpr& b, uint32_t pos)
    {
        if (!b.lhs || !b.rhs || b.op.size() < 2 || b.op.back() != '=')
            return false;
        const std::string base = b.op.substr(0, b.op.size() - 1);
        const std::optional<std::string> ident = op_ident(base, 1);
        if (!ident || !is_binary_op_ident(*ident))
            return false;
        const std::string lt = type_of(*b.lhs);
        if (is_class_handle_text(lt))
        {
            error(pos, "operator '" + b.op + "' needs a value, not a handle");
            return true;
        }
        std::string name = lt;
        const bool opt = is_optional(lt);
        while (!name.empty() && (name.back() == '&' || name.back() == ' ' || name.back() == '?'))
            name.pop_back();
        const ClassDef* d = class_named(name);
        if (!d)
            return false;
        const ClassDef* owner = nullptr;
        const Method* m = find_fn(*d, *ident, &owner);
        if (opt)
        {
            if (!m)
                return false;
            error(pos, "operator '" + b.op + "' needs a value, not an optional");
            return true;
        }
        const std::string head = "operator '" + b.op + "'";
        if (!m || m->op.empty())
        {
            error(pos, head + " needs 'operator" + base + "' in '" + d->name + "'");
            return true;
        }
        if (owner != d)
        {
            error(pos,
                head + " is not available on '" + d->name + "': 'operator" + base + "' comes from the parent '"
                    + owner->name + "'. Declare 'operator" + base + "' in '" + d->name + "'");
            return true;
        }
        const std::string ret = ret_text(*m);
        if (m->params.size() != 1 || ret != d->name)
        {
            error(pos, head + " needs 'operator" + base + "' of '" + d->name + "' to return '" + d->name + "'");
            return true;
        }
        if (const std::string why = op_rhs_problem(*m, *b.rhs); !why.empty() && why != "a value of another class")
        {
            error(pos, head + " needs a value, not " + why);
            return true;
        }
        return false;
    }

    // The target "t" of an assignment, "++" or "--" is "a[i]" of an index operator that returns a value:
    // the code would write a temporary. Reports it and returns true.
    bool op_index_target_error(const Expr& t, uint32_t pos)
    {
        const Expr* x = &t;
        while (auto* p = std::get_if<ParenExpr>(&x->v))
        {
            if (!p->inner)
                return false;
            x = p->inner.get();
        }
        if (!std::holds_alternative<IndexExpr>(x->v))
            return false;
        const Method* m = op_use(*x);
        if (!m || m->ret.compare(0, 4, "ref ") == 0 || ast::is_ref_type(m->ret))
            return false;
        error(pos, "operator '[]' must return 'ref T' to be assigned");
        return true;
    }

    // The type of the use "e" of an operator: the type that the operator method returns. A compare
    // gives "bool". Empty: "e" is no operator use.
    std::string op_result_type(const Expr& e) const
    {
        const Method* m = op_use(e);
        if (!m)
            return {};
        if (is_compare_op_ident(m->name))
            return "bool";
        std::string t = ret_text(*m);
        while (!t.empty() && (t.back() == '&' || t.back() == ' '))
            t.pop_back();
        return t;
    }

    // ---- checked integer divide and modulo (runtime/arith.h)
    enum class NumKind
    {
        Unknown, // the pass does not know the type: the helper decides at C++ compile time
        Int,
        Float,
        Other // another known type, for example a class with its own operator
    };

    // The kind of a type text.
    NumKind type_kind(std::string t) const
    {
        if (t.compare(0, 6, "const ") == 0)
            t.erase(0, 6);
        while (!t.empty() && (t.back() == '&' || t.back() == ' '))
            t.pop_back();
        if (t.empty())
            return NumKind::Unknown;
        if (t == "float" || t == "double" || t == "long double")
            return NumKind::Float;
        static const std::set<std::string> ints = {"bool", "char", "wchar_t", "signed char", "unsigned char", "short",
            "unsigned short", "int", "unsigned", "unsigned int", "long", "unsigned long", "long long",
            "unsigned long long", "size_t", "std::size_t", "ptrdiff_t", "std::ptrdiff_t", "ssize_t"};
        if (ints.count(t))
            return NumKind::Int;
        if (const char* cpp = builtin_cpp_name(t))
        {
            const std::string c = cpp;
            return c == "float" || c == "double" ? NumKind::Float : NumKind::Int;
        }
        if ((t.compare(0, 3, "int") == 0 || t.compare(0, 4, "uint") == 0) && t.size() > 5
            && t.compare(t.size() - 2, 2, "_t") == 0)
            return NumKind::Int; // int32_t, uint64_t, intptr_t and the others
        // A script class has its own operator. Any other name can be an alias of an integer type.
        return class_named(t) ? NumKind::Other : NumKind::Unknown;
    }

    // The text of an integer literal without a sign, or empty for any other expression.
    static std::string int_literal(const Expr& e)
    {
        const auto* n = std::get_if<NumberExpr>(&e.v);
        if (!n)
            return {};
        const std::string& t = n->text;
        const bool hex = t.size() > 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X');
        if (!hex && t.find_first_of(".eEfF") != std::string::npos)
            return {};
        return t;
    }

    // The literal "e" without parentheses and a sign: gives the sign too.
    static const Expr& strip_sign(const Expr& e, bool& negative)
    {
        const Expr* x = &e;
        for (;;)
        {
            if (auto* p = std::get_if<ParenExpr>(&x->v); p && p->inner)
                x = p->inner.get();
            else if (auto* u = std::get_if<UnaryExpr>(&x->v); u && !u->postfix && u->operand && (u->op == "-" || u->op == "+"))
            {
                negative = negative != (u->op == "-");
                x = u->operand.get();
            }
            else
                return *x;
        }
    }

    // True when the divisor is an integer literal that is not 0 and not -1: no check is needed.
    static bool safe_divisor_literal(const Expr& e)
    {
        bool negative = false;
        const std::string t = int_literal(strip_sign(e, negative));
        if (t.empty())
            return false;
        size_t i = t.size() > 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X' || t[1] == 'b' || t[1] == 'B') ? 2 : 0;
        std::string digits; // without the prefix, the separators and the suffix
        for (; i < t.size(); ++i)
            if (t[i] != '\'' && t[i] != 'u' && t[i] != 'U' && t[i] != 'l' && t[i] != 'L')
                digits += t[i];
        const size_t first = digits.find_first_not_of('0');
        if (first == std::string::npos)
            return false; // zero
        return !(negative && digits.compare(first, std::string::npos, "1") == 0);
    }

    // The kind of the operand "e" (before its lowering).
    NumKind num_kind(const Expr& e) const
    {
        bool negative = false;
        const Expr& x = strip_sign(e, negative);
        if (const auto* n = std::get_if<NumberExpr>(&x.v))
            return int_literal(x).empty() && !n->text.empty() ? NumKind::Float : NumKind::Int;
        if (const std::string* n = simple_name(x); n && !cx.locals.count(*n) && cx.cls && cx.allow_this)
            if (const Property* p = find_prop(*cx.cls, *n))
                return type_kind(p->script_type.empty() ? p->type : p->script_type);
        if (const MemberExpr* m = this_member(x); m && cx.allow_this && cx.cls)
        { // this.name: a field or a property of the class
            if (const Field* f = find_field(*cx.cls, m->name))
                return type_kind(type_text(*f));
            if (const Property* p = find_prop(*cx.cls, m->name))
                return type_kind(p->script_type.empty() ? p->type : p->script_type);
        }
        return type_kind(type_of(x));
    }

    // True when "l op r" needs hot::div or hot::mod. "r" is the right operand.
    bool div_checked(NumKind l, const Expr& r) const
    {
        const NumKind rk = num_kind(r);
        if (l == NumKind::Float || l == NumKind::Other || rk == NumKind::Float || rk == NumKind::Other)
            return false;
        return !safe_divisor_literal(r);
    }

    // The script type of an expression before its lowering. Empty: not known.
    std::string type_of(const Expr& e) const
    {
        if (auto* ix = std::get_if<IndexExpr>(&e.v))
        {
            if (const std::string t = op_result_type(e); !t.empty())
                return t;
            return ix->obj ? element_type(type_of(*ix->obj)) : std::string();
        }
        if (std::holds_alternative<ThisExpr>(e.v))
            return cx.allow_this && cx.cls ? cx.cls->name : std::string();
        if (const std::string* n = simple_name(e))
        {
            if (!cx.locals.count(*n))
            { // a field of the class (S20b)
                const Field* f = cx.allow_this && cx.cls ? find_field(*cx.cls, *n) : nullptr;
                return f ? type_text(*f) : std::string();
            }
            auto it = cx.local_type.find(*n);
            return it == cx.local_type.end() ? std::string() : it->second;
        }
        if (auto* nw = std::get_if<NewExpr>(&e.v))
            return class_named(nw->type) ? nw->type : std::string();
        if (auto* p = std::get_if<ParenExpr>(&e.v))
            return type_of(*p->inner);
        if (auto* aw = std::get_if<UnaryExpr>(&e.v); aw && !aw->postfix && aw->op == "await" && aw->operand)
            return task_value_type(type_of(*aw->operand)); // "await" of a "Task<T>" gives T
        if (auto* tn = std::get_if<TernaryExpr>(&e.v))
        { // both branches must have the same known type
            if (!tn->a || !tn->b)
                return {};
            const std::string ta = type_of(*tn->a);
            return !ta.empty() && ta == type_of(*tn->b) ? ta : std::string();
        }
        if (auto* u = std::get_if<UnaryExpr>(&e.v))
            return u->postfix ? std::string() : op_result_type(e);
        if (auto* b = std::get_if<BinaryExpr>(&e.v))
        {
            if (b->op != "??" && b->lhs && b->rhs)
                return op_result_type(e);
            if (b->op != "??" || !b->lhs || !b->rhs)
                return {};
            const std::string t = common_type(type_of(*b->lhs), type_of(*b->rhs));
            const ClassDef* d = class_named(t);
            return d && !d->is_value ? t : std::string();
        }
        if (auto* tt = std::get_if<TypeTestExpr>(&e.v))
        {
            const ClassDef* d = tt->is_as ? class_named(tt->type) : nullptr;
            if (tt->is_as && iface_named(tt->type))
                return tt->type;
            return d && !d->is_value ? tt->type : std::string();
        }
        if (auto* nm = std::get_if<NameExpr>(&e.v))
        { // a.b.c with a local head, before member_chain
            if (nm->parts.size() < 2 || !cx.locals.count(nm->parts[0]))
                return {};
            auto it = cx.local_type.find(nm->parts[0]);
            std::string t = it == cx.local_type.end() ? std::string() : it->second;
            for (size_t i = 1; i < nm->parts.size() && !t.empty(); ++i)
                t = member_type(t, nm->parts[i]);
            return t;
        }
        if (auto* m = std::get_if<MemberExpr>(&e.v))
        {
            if (std::holds_alternative<BaseExpr>(m->obj->v))
                return {};
            const std::string mt = member_type(type_of(*m->obj), m->name);
            return m->safe ? safe_result_type(mt) : mt;
        }
        if (auto* c = std::get_if<CallExpr>(&e.v))
        {
            if (const std::string* n = simple_name(*c->callee))
            {
                if (cx.locals.count(*n))
                    return {};
                if (const Method* st = bare_function(*n))
                    return st->script_ret;
                const Method* fn = cx.cls ? find_fn(*cx.cls, *n) : nullptr;
                return fn ? fn->script_ret : std::string();
            }
            if (auto* m = std::get_if<MemberExpr>(&c->callee->v))
            {
                const std::string ot = type_of(*m->obj);
                if (m->name == "front" || m->name == "back" || m->name == "at")
                    if (const std::string el = element_type(ot); !el.empty())
                        return el;
                if (const InterfaceDef* id = iface_named(ot))
                {
                    const Method* im = iface_method(*id, m->name);
                    return im ? ret_text(*im) : std::string();
                }
                const ClassDef* d = class_named(ot);
                const Method* fn = d ? find_fn(*d, m->name) : nullptr;
                const std::string rt = fn ? fn->script_ret : std::string();
                return m->safe ? safe_result_type(rt) : rt;
            }
            if (auto* nm = std::get_if<NameExpr>(&c->callee->v))
            {
                if (nm->parts.size() < 2)
                    return {};
                if (const Method* st = scope_static(*nm))
                    return st->script_ret;
                std::string t = type_of(NameExprView(*nm, nm->parts.size() - 1));
                const std::string& last = nm->parts.back();
                if (last == "front" || last == "back" || last == "at")
                    if (const std::string el = element_type(t); !el.empty())
                        return el;
                if (const InterfaceDef* id = iface_named(t))
                {
                    const Method* im = iface_method(*id, nm->parts.back());
                    return im ? ret_text(*im) : std::string();
                }
                const ClassDef* d = class_named(t);
                const Method* fn = d ? find_fn(*d, nm->parts.back()) : nullptr;
                return fn ? fn->script_ret : std::string();
            }
        }
        return {};
    }

    // True when the script class "anc" is the class "c" or one of its script parents.
    bool is_ancestor_or_same(const std::string& anc, const std::string& c) const
    {
        int depth = 0;
        for (const ClassDef* d = class_named(c); d && depth < 64; d = parent_of(*d), ++depth)
            if (d->name == anc)
                return true;
        return false;
    }

    // The type of "x ?? y" before its lowering. An unknown side gives the type of the other side.
    // Two different script classes give the nearest common script parent of the left class. When
    // there is no such parent, the type is the left type.
    std::string common_type(const std::string& l, const std::string& r) const
    {
        if (l.empty())
            return r;
        if (r.empty() || l == r)
            return l;
        int depth = 0;
        for (const ClassDef* d = class_named(l); d && depth < 64; d = parent_of(*d), ++depth)
            if (is_ancestor_or_same(d->name, r))
                return d->name;
        return l;
    }

    // The type of the member "name" of an object of the type "t": a field or a property.
    std::string member_type(const std::string& t, const std::string& name) const
    {
        const ClassDef* d = class_named(t);
        if (!d)
            return {};
        if (const Field* f = find_field(*d, name))
            return type_text(*f);
        if (const Property* p = find_prop(*d, name))
            return p->script_type.empty() ? p->type : p->script_type;
        return {};
    }

    // The type of the first "n" parts of a name, as a temporary expression.
    static Expr NameExprView(const NameExpr& nm, size_t n)
    {
        NameExpr head;
        head.parts.assign(nm.parts.begin(), nm.parts.begin() + static_cast<std::ptrdiff_t>(n));
        Expr e;
        e.v = std::move(head);
        return e;
    }

    // Records the type of a local. An empty type removes an older entry of the same name.
    void declare(const std::string& name, const std::string& type)
    {
        cx.locals.insert(name);
        cx.weak_locals.erase(name); // a "[weak]" local adds the name again after this call
        cx.value_locals.erase(name); // a box local adds the name again after this call
        if (type.empty())
            cx.local_type.erase(name);
        else
            cx.local_type[name] = object_type(type);
    }

    // ---- weak handles (task 38)
    // True for the next name() or member() call when the node is the target of "=". A target
    // is not locked: the assignment writes the weak_ptr itself.
    bool weak_target = false;
    bool target_next = false; // the same for the next expr() call

    // A field of the class "c" (or of a script parent) that has "[weak]".
    bool weak_field_of(const ClassDef& c, const std::string& n) const
    {
        const Field* f = find_field(c, n);
        return f && f->weak;
    }

    // A bare name that names a "[weak]" parameter, local or field of the current class.
    bool weak_bare(const std::string& n) const
    {
        if (cx.locals.count(n))
            return cx.weak_locals.count(n) != 0;
        // A script member wins over a module variable (the rule of name_inner).
        if (!cx.cls || !cx.allow_this || (is_module_var(n) && !is_script_member(n)))
            return false;
        return cx.weak_fields.count(n) != 0 || weak_field_of(*cx.cls, n);
    }

    // The member "name" of an object of the script type "t" is a "[weak]" field.
    bool weak_member(const std::string& t, const std::string& name) const
    {
        const ClassDef* d = class_named(t);
        return d && weak_field_of(*d, name);
    }

    // The name of the weak handle that the expression reads (the root of "x", "x.f", "x[i].f"
    // and "this.x"), else an empty string. Call it before the lowering of the expression.
    std::string weak_root(const Expr& e) const
    {
        if (auto* n = std::get_if<NameExpr>(&e.v))
        {
            if (n->parts.empty() || !n->template_args.empty())
                return {};
            if (weak_bare(n->parts[0]))
                return n->parts[0];
            // "a.w": the field "w" of the script object "a"
            for (size_t i = 1; i < n->parts.size(); ++i)
            {
                const Expr head = NameExprView(*n, i);
                if (weak_member(type_of(head), n->parts[i]))
                    return n->parts[i];
            }
            return {};
        }
        if (auto* m = std::get_if<MemberExpr>(&e.v))
        {
            if (!m->obj)
                return {};
            if (std::holds_alternative<ThisExpr>(m->obj->v))
                return cx.cls && cx.allow_this && weak_field_of(*cx.cls, m->name) ? m->name : std::string();
            if (weak_member(type_of(*m->obj), m->name))
                return m->name;
            return weak_root(*m->obj);
        }
        if (auto* ix = std::get_if<IndexExpr>(&e.v))
            return ix->obj ? weak_root(*ix->obj) : std::string();
        if (auto* p = std::get_if<ParenExpr>(&e.v))
            return p->inner ? weak_root(*p->inner) : std::string();
        return {};
    }

    // True when the expression is the weak handle itself: "x", "this.x" or "obj.x".
    bool is_weak_handle(const Expr& e) const
    {
        if (auto* n = std::get_if<NameExpr>(&e.v))
        {
            if (n->parts.size() == 1)
                return n->template_args.empty() && weak_bare(n->parts[0]);
            if (n->parts.size() < 2 || !n->template_args.empty())
                return false;
            const Expr head = NameExprView(*n, n->parts.size() - 1);
            return weak_member(type_of(head), n->parts.back());
        }
        if (auto* m = std::get_if<MemberExpr>(&e.v))
        {
            if (!m->obj)
                return false;
            if (std::holds_alternative<ThisExpr>(m->obj->v))
                return cx.cls && cx.allow_this && weak_field_of(*cx.cls, m->name);
            return weak_member(type_of(*m->obj), m->name);
        }
        return false;
    }

    // A reference into a weak handle dangles: the locked handle ends with the statement.
    void weak_ref_error(uint32_t pos, const std::string& name)
    {
        error(pos,
            "a reference into the weak handle '" + name
                + "' dangles: the lock ends with the statement. Copy the handle to a local first: 'T h = "
                + name + ";'");
    }

    // "new C()" as the whole value of a weak handle. The new object has no strong handle, so it
    // dies at once and the weak handle reads null.
    void reject_weak_new(const ExprP& e)
    {
        const Expr* x = e.get();
        while (x)
        {
            if (auto* p = std::get_if<ParenExpr>(&x->v))
                x = p->inner.get();
            else
                break;
        }
        if (!x)
            return;
        if (auto* n = std::get_if<NewExpr>(&x->v))
            error(n->pos ? n->pos : pos_of(*x),
                "'new' cannot be stored in a weak handle: the new object dies at once and the handle is null. "
                "Store it in a strong handle first: 'T h = new T(); w = h;'");
    }

    // The C++ type of a script type text, by TypeModel::cpp_storage(). An error is reported at "pos"
    // and the type stays as it is. The model must exist.
    std::string cpp_type(const std::string& type, TypeModel::StorageUse use, uint32_t pos)
    {
        std::string message;
        std::string r = cx.types->cpp_storage(type, use, &message);
        if (!message.empty())
        {
            error(pos, message);
            return type;
        }
        return r;
    }

    // The C++ type of a "[weak] T" parameter. The translator can rewrite "type" to it before the pass.
    std::string weak_param_type(const Param& p) const
    {
        if (p.type.find("weak_ptr<") != std::string::npos)
            return p.type;
        if (!cx.types)
            return {};
        const std::string& t = p.script_type.empty() ? p.type : p.script_type;
        // A type that is no handle is the weak storage already (a custom weak_handle template).
        return cx.types->is_handle(t) ? cx.types->weak_storage(t) : p.type;
    }

    // ---- box uses (V7)
    // A field of the class "c" (or of a script parent) that has box.
    bool value_field_of(const ClassDef& c, const std::string& n) const
    {
        const Field* f = find_field(c, n);
        return f && f->value;
    }

    // A bare name that names a box parameter, local or field of the current class.
    bool value_bare(const std::string& n) const
    {
        if (cx.locals.count(n))
            return cx.value_locals.count(n) != 0;
        if (!cx.cls || !cx.allow_this || (is_module_var(n) && !is_script_member(n)))
            return false;
        return cx.value_fields.count(n) != 0 || value_field_of(*cx.cls, n);
    }

    // The script function that a call names, when the pass knows it (the rules of type_of).
    const Method* callee_fn(const CallExpr& c) const
    {
        if (!c.callee)
            return nullptr;
        if (const std::string* n = simple_name(*c.callee))
            return cx.locals.count(*n) || !cx.cls ? nullptr : find_fn(*cx.cls, *n);
        if (auto* m = std::get_if<MemberExpr>(&c.callee->v))
        {
            if (!m->obj || std::holds_alternative<BaseExpr>(m->obj->v))
                return nullptr;
            const ClassDef* d = class_named(type_of(*m->obj));
            return d ? find_fn(*d, m->name) : nullptr;
        }
        if (auto* nm = std::get_if<NameExpr>(&c.callee->v))
        {
            if (nm->parts.size() < 2)
                return nullptr;
            const ClassDef* d = class_named(type_of(NameExprView(*nm, nm->parts.size() - 1)));
            return d ? find_fn(*d, nm->parts.back()) : nullptr;
        }
        return nullptr;
    }

    // True when the expression is a box storage or reads one: a name, "this.f", "obj.f" of
    // a box field or a box local, or a call of a function that returns "C" (a bare script class).
    // Call it before the lowering of the expression.
    bool is_value_ref(const Expr& e) const
    {
        if (auto* n = std::get_if<NameExpr>(&e.v))
        {
            if (n->parts.empty() || !n->template_args.empty())
                return false;
            if (n->parts.size() == 1)
                return value_bare(n->parts[0]);
            const ClassDef* d = class_named(type_of(NameExprView(*n, n->parts.size() - 1)));
            return d && value_field_of(*d, n->parts.back());
        }
        if (auto* m = std::get_if<MemberExpr>(&e.v))
        {
            if (!m->obj)
                return false;
            if (std::holds_alternative<ThisExpr>(m->obj->v))
                return cx.cls && cx.allow_this && value_field_of(*cx.cls, m->name);
            const ClassDef* d = class_named(type_of(*m->obj));
            return d && value_field_of(*d, m->name);
        }
        if (auto* p = std::get_if<ParenExpr>(&e.v))
            return p->inner && is_value_ref(*p->inner);
        if (auto* c = std::get_if<CallExpr>(&e.v))
        {
            const Method* fn = callee_fn(*c);
            return fn && fn->ret_value;
        }
        if (auto* n = std::get_if<NewExpr>(&e.v))
            return new_makes_box(*n);
        if (std::holds_alternative<BinaryExpr>(e.v) || std::holds_alternative<UnaryExpr>(e.v)
            || std::holds_alternative<IndexExpr>(e.v))
        { // an operator of a script class that returns "C" gives a box
            const Method* m = op_use(e);
            return m && m->ret_value;
        }
        return false;
    }

    // True when "new C(...)" gives a box: C is a script class and the text has no "$".
    bool new_makes_box(const NewExpr& n) const
    {
        const ClassDef* d = n.type.compare(0, 1, "$") != 0 ? class_named(n.type) : nullptr;
        return d && !d->is_value;
    }

    // The element type E of hot::Value<E> for the script type "t". Empty: "t" cannot be a box.
    std::string value_elem(const std::string& t) const
    {
        const std::string head = "hot::Value<";
        if (t.compare(0, head.size(), head) == 0 && t.back() == '>')
            return t.substr(head.size(), t.size() - head.size() - 1); // the storage type already
        if (!cx.types)
            return {};
        const std::string v = cx.types->value_storage(t);
        return v.size() > head.size() ? v.substr(head.size(), v.size() - head.size() - 1) : std::string();
    }

    // A box goes to a handle: the code must copy the object into a box first, or the target
    // must be a box itself. The text is the hint of the error.
    void value_to_handle_error(uint32_t pos)
    {
        error(pos,
            "a value cannot convert to a handle: the box owns the object. Declare the target without '$' to copy "
            "it");
    }

    // Prepares "e" for a "C" (a bare script class) target (call it before the lowering of "e"). "new C()" becomes
    // "hot::Value<E>(new C())": the new object goes into the box without a copy. A box stays: the
    // copy or the move of the box does the work. Any other expression is a handle (or it is not
    // known): hot::as_value<E>(e) copies the object, and a null handle throws.
    void value_wrap(ExprP& e, const std::string& type)
    {
        if (!e)
            return;
        const std::string elem = value_elem(object_type(type));
        if (elem.empty())
            return;
        const uint32_t pos = pos_of(*e);
        if (std::holds_alternative<NullExpr>(e->v))
        {
            error(pos, kNullValueError);
            return;
        }
        coerce(e, "$" + elem); // a bare "this" becomes the handle of the object
        if (is_value_ref(*e))
            return;
        const bool fresh = std::holds_alternative<NewExpr>(e->v); // "new $C()": the box takes the object
        e = call_of((fresh ? "hot::Value<" : "hot::as_value<") + elem + ">", args_of(std::move(e)), pos);
    }

    // The expression "e" goes to the target of the script type "type", which is not a box.
    void value_not_to_handle(const ExprP& e, const std::string& type)
    {
        if (!e || type.empty())
            return;
        const ClassDef* d = class_named(object_type(type));
        if (d && !d->is_value && is_value_ref(*e))
            value_to_handle_error(pos_of(*e));
    }

    // "hot::lock(e)": the read of a weak handle.
    void lock(ExprP& e)
    {
        const uint32_t pos = pos_of(*e);
        std::vector<ExprP> a;
        a.push_back(std::move(e));
        e = call_of("hot::lock", std::move(a), pos);
    }

    // "hot::shell_of<C_Shell>(obj, "C")" for the lowered text of the object.
    std::string shell_text(const ClassDef& d, const std::string& obj_text) const
    {
        return "hot::shell_of<" + d.name + "_Shell>(" + obj_text + ", \"" + d.name + "\")";
    }

    // The name of the interface when "type" is an interface value type ("I" or "const I&"). A
    // "I&" that is not const is a reference to the handle: nothing converts to it.
    std::string iface_target(const std::string& type) const
    {
        const InterfaceDef* d = iface_named(type);
        if (!d)
            return {};
        std::string t = type;
        while (!t.empty() && (t.back() == ' ' || t.back() == '\t'))
            t.pop_back();
        if (!t.empty() && t.back() == '&' && t.compare(0, 6, "const ") != 0)
            return {};
        return d->name;
    }

    // The expression "e" goes to a target of the interface type "iface" (D5). The result is
    // "hot::to_iface<I__iface>(e)". "null" becomes the empty handle. A bare "this" first becomes
    // the handle of the class. A class that does not list the interface is an error. A value
    // of an unknown type is wrapped: to_iface keeps an interface handle as it is.
    void to_iface_conv(ExprP& e, const std::string& iface)
    {
        const uint32_t pos = pos_of(*e);
        const std::string storage = cx.types ? cx.types->iface_storage(iface) : "hot::Iface<" + iface + "__iface>";
        if (std::holds_alternative<NullExpr>(e->v))
        {
            e = raw(storage + "()", pos);
            return;
        }
        if (std::holds_alternative<ThisExpr>(e->v))
        {
            if (!cx.allow_this || !cx.cls)
                return;
            if (!class_lists_iface(*cx.cls, iface))
            {
                error(pos, "the class '" + cx.cls->name + "' does not implement the interface '" + iface + "'");
                return;
            }
            coerce(e, cx.cls->name); // "this" becomes the handle of the object
        }
        else if (const std::string t = object_type(type_of(*e)); !t.empty())
        {
            if (t == iface)
                return;
            if (iface_named(t))
            {
                error(pos, "the interface '" + t + "' does not convert to the interface '" + iface + "': use 'as "
                    + iface + "'");
                return;
            }
            if (const ClassDef* d = class_named(t))
            {
                if (d->is_value)
                {
                    error(pos, "the struct '" + t + "' does not convert to the interface '" + iface + "'");
                    return;
                }
                if (!class_lists_iface(*d, iface))
                {
                    error(pos, "the class '" + t + "' does not implement the interface '" + iface + "'");
                    return;
                }
            }
        }
        e = call_of("hot::to_iface<" + iface + "__iface>", args_of(std::move(e)), pos);
    }

    // The type text of the handle of a script class: "C" gives "$C". Another text stays.
    std::string handle_form(const std::string& type) const
    {
        const ClassDef* d = class_named(type);
        return d && !d->is_value && type.compare(0, 1, "$") != 0 ? "$" + type : type;
    }

    // The class T of a handle type text: "$T", "const $T&", "std::shared_ptr<T>" or a bare native handle
    // class. Empty when the text is not a handle type. A bare script class is a box, so it is not a handle.
    std::string handle_class(const std::string& type_in) const
    {
        std::string t = type_in;
        auto trim = [&]()
        {
            while (!t.empty() && (t.back() == ' ' || t.back() == '\t' || t.back() == '&'))
                t.pop_back();
            while (!t.empty() && (t[0] == ' ' || t[0] == '\t'))
                t.erase(0, 1);
        };
        trim();
        if (t.compare(0, 6, "const ") == 0)
            t.erase(0, 6);
        trim();
        if (t.empty())
            return {};
        if (t[0] == '$')
            t.erase(0, 1);
        else if (const size_t sp = t.find("shared_ptr<"); sp != std::string::npos && t.back() == '>')
        {
            t = t.substr(sp + 11, t.size() - sp - 12);
            trim();
        }
        else if (class_named(t) || !cx.types || !cx.types->is_handle(t))
            return {};
        for (char ch : t)
            if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' || ch == ':' || ch == '.'))
                return {};
        return t;
    }

    // True when "this", a "&x" or a "C*" typed name or member gives the expression (a raw pointer).
    bool raw_pointer_source(const Expr& e) const
    {
        if (std::holds_alternative<ThisExpr>(e.v))
            return cx.allow_this && cx.cls;
        if (auto* p = std::get_if<ParenExpr>(&e.v))
            return p->inner && raw_pointer_source(*p->inner);
        std::string t;
        if (auto* u = std::get_if<UnaryExpr>(&e.v); u && !u->postfix && u->operand)
        {
            if (u->op == "&")
                return true;
            if (u->op.size() > 2 && u->op.front() == '(' && u->op.back() == ')')
            { // a cast "(T*)x" to a raw pointer type gives a raw pointer
                std::string c = u->op.substr(1, u->op.size() - 2);
                while (!c.empty() && c.back() == ' ')
                    c.pop_back();
                return c.size() > 1 && c.back() == '*' && c[0] != '$';
            }
            if (u->op == "*")
            { // "*pp" of a "T**" gives a raw pointer
                t = type_of(*u->operand);
                while (!t.empty() && t.back() == ' ')
                    t.pop_back();
                return t.size() > 2 && t.compare(t.size() - 2, 2, "**") == 0 && t[0] != '$';
            }
        }
        t = type_of(e);
        while (!t.empty() && (t.back() == ' ' || t.back() == '\t'))
            t.pop_back();
        return t.size() > 1 && t.back() == '*' && t[0] != '$';
    }

    // The class C of "$C" can give a shared pointer from "this" through a weak source: a script
    // class (its shell has hot::weak_ref_traits), or a native class with std::enable_shared_from_this.
    bool has_weak_source(const std::string& cls) const
    {
        const ClassDef* d = class_named(cls);
        if (d)
            return !d->is_value;
        return cx.types && cx.types->has_shared_from_this(cls);
    }

    // "$C x = p" with a raw pointer "p" is an error, unless C has a weak source.
    void check_raw_to_handle(const ExprP& e, const std::string& type)
    {
        if (!e || type.empty() || !cx.types)
            return;
        const std::string cls = handle_class(type);
        if (cls.empty() || !raw_pointer_source(*e) || has_weak_source(cls))
            return;
        error(pos_of(*e), kRawToSharedError);
    }

    // "$Derived x = b" with a "$Base" value "b" needs a cast. "$Base x = d" needs nothing.
    void check_downcast(const ExprP& e, const std::string& type)
    {
        if (!e || type.empty() || !cx.types)
            return;
        const std::string target = handle_class(type);
        if (target.empty() || std::holds_alternative<NullExpr>(e->v) || raw_pointer_source(*e))
            return;
        const std::string source = handle_class(type_of(*e));
        if (source.empty() || source == target || !cx.types->derives_from(target, source))
            return;
        error(pos_of(*e),
            "cannot convert the base handle '$" + source + "' to the derived handle '$" + target
                + "' without a cast; write ($" + target + ")x");
    }

    // "h.reset(p)" on a "$C" or a "std::shared_ptr<C>" with a raw pointer "p" is an error.
    void check_reset(const CallExpr& c)
    {
        if (c.args.size() != 1 || !c.args[0] || !c.callee || !cx.types)
            return;
        std::string obj_type;
        bool weak = false;
        if (auto* m = std::get_if<MemberExpr>(&c.callee->v); m && m->name == "reset" && m->obj)
        {
            if (!std::holds_alternative<BaseExpr>(m->obj->v))
            {
                obj_type = type_of(*m->obj);
                weak = is_weak_handle(*m->obj);
            }
        }
        else if (auto* nm = std::get_if<NameExpr>(&c.callee->v);
                 nm && nm->parts.size() >= 2 && nm->parts.back() == "reset" && nm->template_args.empty())
        {
            const Expr head = NameExprView(*nm, nm->parts.size() - 1);
            obj_type = type_of(head);
            weak = is_weak_handle(head);
        }
        if (weak || handle_class(obj_type).empty() || !raw_pointer_source(*c.args[0]))
            return;
        error(pos_of(*c.args[0]), kRawToSharedError);
    }

    // Where a handle type is expected, a bare "this" becomes a handle (design 7.10.1).
    void coerce(ExprP& e, const std::string& type)
    {
        if (e && is_optional(type) && std::holds_alternative<NullExpr>(e->v))
        { // "null" for a "T?" is the empty optional
            e = raw("std::nullopt", pos_of(*e));
            return;
        }
        if (e && std::holds_alternative<NullExpr>(e->v) && type.compare(0, 1, "$") != 0)
            if (const ClassDef* sd = class_named(type); sd && sd->is_value)
            { // a struct is a value: it is never null
                error(pos_of(*e), kNullValueError);
                return;
            }
        if (e && !type.empty())
            if (const std::string iface = iface_target(type); !iface.empty())
            {
                to_iface_conv(e, iface);
                return;
            }
        check_raw_to_handle(e, type);
        check_downcast(e, type);
        if (!e || type.empty() || !cx.types || !cx.allow_this)
            return;
        if (!std::holds_alternative<ThisExpr>(e->v))
            return;
        const std::string h = cx.types->cpp_storage(handle_form(type), TypeModel::StorageUse::Local);
        if (h == type)
            return; // not a handle type: "this" stays the pointer
        const uint32_t pos = std::get<ThisExpr>(e->v).pos;
        std::vector<ExprP> a;
        a.push_back(raw("__self", 0));
        if (cx.handle_this)
            *cx.handle_this = true;
        e = call_of("hot::handle_from_this<" + h + ">", std::move(a), pos);
    }

    // The arguments of a call of the script function "fn" (null: no information).
    void coerce_args(CallExpr& c, const Method* fn, size_t first)
    {
        if (fn)
            coerce_args(c, fn->params, first);
    }

    void coerce_args(CallExpr& c, const std::vector<Param>& params, size_t first)
    {
        for (size_t i = first; i < c.args.size() && i - first < params.size(); ++i)
        {
            const Param& p = params[i - first];
            const std::string& t = p.script_type.empty() ? p.type : p.script_type;
            if (p.value)
            { // "C" (a bare script class): the argument is copied into the parameter
                value_wrap(c.args[i], t);
                continue;
            }
            if (!p.weak)
                value_not_to_handle(c.args[i], t);
            if (p.weak)
            { // "null" for a "[weak] T" parameter: the empty weak_ptr (a weak_ptr has no nullptr form)
                reject_weak_new(c.args[i]);
                if (c.args[i] && std::holds_alternative<NullExpr>(c.args[i]->v))
                {
                    const std::string w = weak_param_type(p);
                    if (!w.empty())
                        c.args[i] = raw(w + "()", pos_of(*c.args[i]));
                }
                else if (!p.script_type.empty())
                {
                    coerce(c.args[i], p.script_type); // "this" becomes the strong handle of the script type
                }
                continue;
            }
            if (!is_ref_type(t))
            {
                coerce(c.args[i], t);
            }
            else if (c.args[i] && std::holds_alternative<ThisExpr>(c.args[i]->v) && class_named(object_type(t)))
            {
                // "T&" of a script class is a reference to a handle. "this" is not a variable.
                error(std::get<ThisExpr>(c.args[i]->v).pos,
                    "'this' cannot bind to the reference parameter '" + p.name + "' of the script class '"
                        + object_type(t) + "'. Copy it to a local first: '$" + object_type(t) + " me = this;'");
            }
        }
    }

    // The native base of the root of the script family of the current class. It is empty for a
    // class without a native base.
    std::string root_native_base() const
    {
        if (!cx.native_base.empty() || !cx.cls)
            return cx.native_base;
        const ClassDef* r = cx.cls;
        while (const ClassDef* p = class_named(r->base))
        {
            if (p == r)
                break;
            r = p;
        }
        return r->base;
    }

    // The class whose properties "obj.P" can name, from the type "t" of obj. A script class
    // gives itself. A known native type gives nullptr (S20b): "obj.P" is the native member.
    // The native base of the family is the exception: the object can be a script object of the
    // class, so the old name rule checks it at run time. An unknown type keeps the old rule too.
    const ClassDef* prop_class(const std::string& t) const
    {
        std::string base = t;
        while (!base.empty() && (base.back() == '*' || base.back() == '&' || base.back() == ' '))
            base.pop_back();
        if (base.rfind("const ", 0) == 0)
            base = base.substr(6);
        if (const ClassDef* td = class_named(base))
            return td;
        if (!t.empty() && base != root_native_base())
            return nullptr;
        return cx.cls;
    }

    // The property that "this.P", a bare "P" or "obj.P" names. A local hides a bare "P".
    // "obj.P" uses the type of obj (see prop_class). An unknown type keeps the name rule: P
    // must be a property of the current class.
    std::optional<PropRef> prop_ref(Expr& e) const
    {
        const std::string* n = nullptr;
        ExprP* obj = nullptr;
        const ClassDef* d = cx.cls;
        if (const MemberExpr* m = this_member(e))
        {
            n = &m->name;
        }
        else if (auto* m2 = std::get_if<MemberExpr>(&e.v))
        {
            if (std::holds_alternative<BaseExpr>(m2->obj->v))
                return std::nullopt;
            n = &m2->name;
            obj = &m2->obj;
            if (!(d = prop_class(type_of(*m2->obj))))
                return std::nullopt;
        }
        else if ((n = simple_name(e)) && cx.locals.count(*n))
        {
            n = nullptr;
        }
        const ClassDef* owner = nullptr;
        if (!n || !d || !find_prop(*d, *n, &owner))
            return std::nullopt;
        return PropRef{*n, obj, owner, obj ? d : nullptr};
    }

    // The checks of an accessor use. Gives the name of the accessor fn: "Cls__get_P".
    std::string accessor(
        const ClassDef& d, const std::string& prop, bool is_get, uint32_t pos, const ClassDef* via = nullptr)
    {
        const char* word = is_get ? "get" : "set";
        const Property* p = find_prop(d, prop);
        if (cx.in_static && !via)
            needs_object_error(prop, pos); // the property of another object ("obj.P") is allowed
        else if (!cx.allow_this && !cx.in_static)
            error(pos, "property '" + prop + "' is not allowed in a field initializer");
        else if (p && !(is_get ? p->has_get : p->has_set))
            error(pos, "property '" + prop + "' has no '" + word + "' accessor");
        else if (p)
        {
            // An accessor with its own access word has its own message.
            const Access a = is_get ? p->get_access : p->set_access;
            check_access(a, d,
                a == p->access ? "'" + prop + "'" : std::string("the '") + word + "' accessor of '" + prop + "'", pos,
                via);
        }
        return d.name + "__" + word + "_" + prop;
    }

    // The first argument of an accessor: "self", or "hot::shell_of<Cls_Shell>(obj, "Cls")"
    // for another object. The helper checks the class of the object at run time.
    ExprP receiver(PropRef& r, uint32_t pos)
    {
        if (!r.obj)
            return raw("__self", 0);
        expr(*r.obj);
        CallExpr c;
        c.pos = pos;
        if (r.cls->is_value)
        { // a box object: the address of the struct
            c.callee = raw("hot::value_ptr", pos);
            c.args.push_back(std::move(*r.obj));
            return make_expr(std::move(c));
        }
        c.callee = raw("hot::shell_of<" + r.cls->name + "_Shell>", pos);
        c.args.push_back(std::move(*r.obj));
        c.args.push_back(raw("\"" + r.cls->name + "\"", 0));
        return make_expr(std::move(c));
    }

    ExprP call_of(std::string callee, std::vector<ExprP> args, uint32_t pos)
    {
        CallExpr c;
        c.pos = pos;
        c.callee = raw(std::move(callee), pos);
        c.args = std::move(args);
        return make_expr(std::move(c));
    }

    template <class... A> static std::vector<ExprP> args_of(A&&... a)
    {
        std::vector<ExprP> v;
        (v.push_back(std::move(a)), ...);
        return v;
    }

    // A read of a property: "Cls__get_P(receiver)".
    ExprP prop_read(PropRef r, uint32_t pos)
    {
        const std::string get = accessor(*r.cls, r.name, true, pos, r.via);
        return call_of(get, args_of(receiver(r, pos)), pos);
    }

    // "P = e", "P op= e", "++P", "--P", "P++" and "P--" on a property of "this" or of
    // another object. Gives false when the target is not a property.
    // In a statement ("discard" is true) the write is a call of the setter. Then a compound
    // write on "this" is "Cls__set_P(self, Cls__get_P(self) op (e))".
    // Else a helper of runtime.h makes the write. The helper gives the C# value and
    // evaluates the object once: hot::prop_set, hot::prop_update or hot::prop_post.
    bool prop_write(ExprP& e, bool discard)
    {
        static const std::map<std::string, std::string> op_names = {{"+", "op_add"}, {"-", "op_sub"}, {"*", "op_mul"},
            {"/", "op_div"}, {"%", "op_mod"}, {"&", "op_and"}, {"|", "op_or"}, {"^", "op_xor"}, {"<<", "op_shl"},
            {">>", "op_shr"}};
        std::optional<PropRef> r;
        std::string op; // empty for "=", else the operator without "="
        bool postfix = false, is_inc = false;
        ExprP value;
        uint32_t pos = 0;
        if (auto* b = std::get_if<BinaryExpr>(&e->v))
        {
            const std::string& o = b->op;
            if (o != "=" && !(o.size() >= 2 && o.back() == '=' && op_names.count(o.substr(0, o.size() - 1))))
                return false;
            member_chain(b->lhs);
            if (!(r = prop_ref(*b->lhs)))
                return false;
            op = o.substr(0, o.size() - 1);
            pos = pos_of(*b->lhs);
            if (op.empty())
            {
                const Property* pr = find_prop(*r->cls, r->name);
                if (pr)
                    coerce(b->rhs, pr->script_type.empty() ? pr->type : pr->script_type);
            }
            expr(b->rhs);
            value = std::move(b->rhs);
        }
        else if (auto* u = std::get_if<UnaryExpr>(&e->v))
        {
            if (u->op != "++" && u->op != "--")
                return false;
            member_chain(u->operand);
            if (!(r = prop_ref(*u->operand)))
                return false;
            op = u->op.substr(0, 1);
            postfix = u->postfix;
            is_inc = true;
            pos = pos_of(*u->operand);
            value = make_expr(NumberExpr{"1", pos});
        }
        else
        {
            return false;
        }
        const bool on_this = !r->obj;
        const std::string get = op.empty() ? std::string() : accessor(*r->cls, r->name, true, pos, r->via);
        const std::string set = accessor(*r->cls, r->name, false, pos, r->via);
        ExprP recv = receiver(*r, pos);
        if (discard && (op.empty() || on_this))
        {
            if (!op.empty())
            {
                BinaryExpr bin;
                bin.op = op;
                bin.lhs = call_of(get, args_of(raw("__self", 0)), pos);
                // "++P" adds 1 without parentheses. "P op= e" puts e in parentheses.
                bin.rhs = is_inc ? std::move(value) : make_expr(ParenExpr{std::move(value), pos});
                bin.pos = pos;
                const Property* pr = find_prop(*r->cls, r->name);
                const NumKind lk = pr ? type_kind(pr->script_type.empty() ? pr->type : pr->script_type) : NumKind::Unknown;
                if ((op == "/" || op == "%") && div_checked(lk, *bin.rhs))
                    value = call_of(op == "/" ? "hot::div" : "hot::mod", args_of(std::move(bin.lhs), std::move(bin.rhs)), pos);
                else
                    value = make_expr(std::move(bin));
            }
            e = call_of(set, args_of(std::move(recv), std::move(value)), pos);
        }
        else if (op.empty())
        {
            e = call_of("hot::prop_set", args_of(raw(set, pos), std::move(recv), std::move(value)), pos);
        }
        else
        {
            const std::string helper = postfix && !discard ? "hot::prop_post" : "hot::prop_update";
            e = call_of(helper + "<hot::" + op_names.at(op) + ">",
                args_of(raw(get, pos), raw(set, pos), std::move(recv), std::move(value)), pos);
        }
        return true;
    }

    void exprs(std::vector<ExprP>& v)
    {
        for (ExprP& x : v)
            expr(x);
    }

    // True for the bare name of a helper of the runtime ("enumerate" or "range"), if nothing else
    // has the name. This is the last choice of the name rules: a local, a parameter, a member, a
    // module variable, a type or a native global of the same name wins.
    bool is_helper_name(const std::string& n) const
    {
        if (n != "enumerate" && n != "range")
            return false;
        if (cx.locals.count(n) || cx.module_vars.count(n) || cx.fns.count(n) || cx.field_type.count(n))
            return false;
        if (cx.props.count(n) || cx.native_members.count(n) || cx.classes.count(n) || cx.script_classes.count(n))
            return false;
        if (cx.cls && (find_field(*cx.cls, n) || find_prop(*cx.cls, n) || find_fn(*cx.cls, n)))
            return false;
        return !(cx.types && (cx.types->is_native_global(n) || cx.types->is_script_class(n)));
    }

    // A script fn that "this.f" or "f" names. The call gets the "self" argument.
    void script_call(CallExpr& c, const std::string& fn, uint32_t pos, uint32_t name_pos = 0)
    {
        const ClassDef* owner = cx.cls;
        const Method* m = find_fn(*cx.cls, fn, &owner);
        c.callee = raw_named("", m ? fn_symbol(*owner, *m) : cx.cls->name + "__" + fn, pos, name_pos);
        c.args.insert(c.args.begin(), raw("__self", 0));
    }

    // True when "T(args)" is a call of the name of a handle class. A handle needs "new".
    // A script class is a handle, unless it is a struct. A native class is a handle
    // when its own stub says so. The text is the hint for the error.
    std::string handle_call_hint(const std::string& n) const
    {
        if (cx.locals.count(n) || cx.fns.count(n))
            return {};
        const ClassDef* d = class_named(n);
        if (d && !d->is_value)
            return "'" + n + "' is a script class. Create it with 'new " + n + "()'.";
        if (!d && cx.types && !cx.script_classes.count(n))
        {
            const TypeRule r = cx.types->lookup(n);
            if (r.kind == Kind::Handle && r.native && r.owner == n && cx.types->is_handle(n))
                return "'" + n + "' is a native class with a handle. Create it with 'new " + n + "()'.";
        }
        return {};
    }

    // "i.M(a)" of an interface value "i": the call "I__M(i, a)" (D9). The helper of the interface
    // method throws a ScriptError for a null handle. An argument that is a class handle goes to an
    // interface parameter with to_iface.
    void iface_call(CallExpr& c, MemberExpr& mm, const InterfaceDef& d)
    {
        const uint32_t pos = pos_of(*mm.obj);
        const Method* m = iface_method(d, mm.name);
        if (!m)
        {
            error(mm.name_pos ? mm.name_pos : pos, "the interface '" + d.name + "' has no method '" + mm.name + "'");
            return;
        }
        if (c.args.size() != m->params.size())
        {
            error(pos, "the interface method '" + d.name + "." + mm.name + "' takes " + std::to_string(m->params.size())
                + " arguments, and the call has " + std::to_string(c.args.size()));
            return;
        }
        const std::string symbol = d.name + "__" + mm.name;
        ExprP recv = std::move(mm.obj);
        expr(recv);
        c.callee = raw(symbol, pos); // "mm" is gone now
        c.args.insert(c.args.begin(), std::move(recv));
        coerce_args(c, m->params, 1);
        for (size_t i = 1; i < c.args.size(); ++i)
            expr(c.args[i]);
    }

    // "a == b" or "a != b" where one side is an interface value: the objects are the same (D9).
    // The result is "hot::iface_same(a, b)". A class handle, an interface value and "null" compare.
    void iface_compare(ExprP& e, BinaryExpr& b)
    {
        const uint32_t pos = b.pos != 0 ? b.pos : pos_of(*b.lhs);
        const bool negate = b.op == "!=";
        // A bare "this" becomes the handle of the object, so the identity has a handle to read.
        for (ExprP* side : {&b.lhs, &b.rhs})
            if (std::holds_alternative<ThisExpr>((*side)->v) && cx.allow_this && cx.cls)
                coerce(*side, cx.cls->name);
        expr(b.lhs);
        expr(b.rhs);
        ExprP res = call_of("hot::iface_same", args_of(std::move(b.lhs), std::move(b.rhs)), pos);
        e = negate ? make_expr(UnaryExpr{"!", std::move(res), false, pos}) : std::move(res); // "b" is gone now
    }

    void call(CallExpr& c)
    {
        check_reset(c);
        if (const std::string* n = simple_name(*c.callee))
        {
            if (const std::string hint = handle_call_hint(*n); !hint.empty())
            {
                error(std::get<NameExpr>(c.callee->v).pos, hint);
                return;
            }
            if (default_banned(*n))
            {
                error(std::get<NameExpr>(c.callee->v).pos, kDefaultBan);
                return;
            }
            if (cx.in_static && cx.fns.count(*n) && !cx.locals.count(*n))
            {
                needs_object_error(*n, std::get<NameExpr>(c.callee->v).pos);
                exprs(c.args);
                return;
            }
            if (const ClassDef* so = nullptr; const Method* st = bare_function(*n, &so))
            {
                static_call(c, *st, so, *n, std::get<NameExpr>(c.callee->v).pos);
                return;
            }
            if (cx.fns.count(*n) && !cx.locals.count(*n))
            {
                const ClassDef* owner = nullptr;
                const Method* fn = find_fn(*cx.cls, *n, &owner);
                if (fn)
                    check_member(fn->access, *owner, *n, std::get<NameExpr>(c.callee->v).pos);
                script_call(c, *n, std::get<NameExpr>(c.callee->v).pos);
                coerce_args(c, fn, 1);
                exprs(c.args);
                return;
            }
        }
        if (const MemberExpr* m = this_member(*c.callee))
        {
            if (const ClassDef* so = nullptr; cx.allow_this && cx.cls && !cx.field_type.count(m->name)
                && !cx.fns.count(m->name) && find_static(*cx.cls, m->name, &so))
            {
                error(m->name_pos ? m->name_pos : pos_of(*m->obj), static_hint(*so, m->name));
                exprs(c.args);
                return;
            }
            if (cx.allow_this && !cx.field_type.count(m->name) && cx.fns.count(m->name))
            {
                const ClassDef* owner = nullptr;
                const Method* fn = find_fn(*cx.cls, m->name, &owner);
                if (fn)
                    check_member(fn->access, *owner, m->name, m->name_pos ? m->name_pos : pos_of(*m->obj));
                script_call(c, m->name, pos_of(*m->obj), m->name_pos);
                coerce_args(c, fn, 1);
                exprs(c.args);
                return;
            }
        }
        // "base.M(args)" in a class with a native base only: a pure method has no body to call.
        if (auto* bm = std::get_if<MemberExpr>(&c.callee->v);
            bm && std::holds_alternative<BaseExpr>(bm->obj->v) && cx.cls && cx.allow_this && !parent_of(*cx.cls)
            && abstract_base_call(bm->name, c.args.size()))
        {
            error(pos_of(*bm->obj), "cannot call the abstract method '" + bm->name + "' through 'base'");
            return;
        }
        // "base.M(args)" in a class with a script parent P (design 4.5): the implementation
        // that P sees, with a direct call. A native virtual M uses the helper "P__base_M".
        if (auto* sm = std::get_if<MemberExpr>(&c.callee->v);
            sm && std::holds_alternative<BaseExpr>(sm->obj->v) && cx.cls && cx.allow_this)
        {
            if (const ClassDef* par = parent_of(*cx.cls))
            {
                const ClassDef* owner = nullptr;
                const Method* fn = find_fn(*par, sm->name, &owner);
                if (fn)
                    check_member(fn->access, *owner, sm->name, sm->name_pos ? sm->name_pos : pos_of(*sm->obj));
                if ((fn && fn->is_abstract) || (!fn && abstract_base_call(sm->name, c.args.size())))
                {
                    error(pos_of(*sm->obj), "cannot call the abstract method '" + sm->name + "' through 'base'");
                    return;
                }
                std::string callee;
                if (fn)
                    callee = owner->name + "__" + sm->name;
                else if (cx.native_virtuals.count(sm->name))
                    callee = par->name + "__base_" + sm->name;
                if (!callee.empty())
                {
                    c.callee = raw(callee, pos_of(*sm->obj));
                    c.args.insert(c.args.begin(), raw("__self", 0));
                    if (fn)
                        coerce_args(c, fn, 1);
                    for (size_t i = 1; i < c.args.size(); ++i)
                        expr(c.args[i]);
                    return;
                }
            }
        }
        // A bare "enumerate(xs)" or "range(n)" is the helper of the runtime, if nothing else has
        // the name. The call is explicit: "hot::enumerate(xs)".
        if (const std::string* n = simple_name(*c.callee); n && is_helper_name(*n))
        {
            const uint32_t pos = std::get<NameExpr>(c.callee->v).pos;
            c.callee = raw_named("hot::", *n, pos, pos);
        }
        // "obj.M(args)" where obj has the type of a script class (not a struct) and M is
        // a script function of it: a direct call of the generation function.
        if (auto* sn = std::get_if<NameExpr>(&c.callee->v))
            if (const ClassDef* so = nullptr; const Method* st = scope_static(*sn, &so))
            {
                static_call(c, *st, so, sn->parts[1], sn->pos);
                return;
            }
        member_chain(c.callee);
        if (auto* mm = std::get_if<MemberExpr>(&c.callee->v))
        {
            if (!std::holds_alternative<ThisExpr>(mm->obj->v) && !std::holds_alternative<BaseExpr>(mm->obj->v))
            {
                if (const InterfaceDef* idef = iface_named(type_of(*mm->obj)))
                {
                    iface_call(c, *mm, *idef);
                    return;
                }
                const ClassDef* d = class_named(type_of(*mm->obj));
                const ClassDef* owner = d;
                const Method* fn =
                    d && !d->is_value && !find_prop(*d, mm->name) ? find_fn(*d, mm->name, &owner) : nullptr;
                if (!fn && d && !find_prop(*d, mm->name))
                    if (const ClassDef* so = nullptr; find_static(*d, mm->name, &so))
                    {
                        error(mm->name_pos ? mm->name_pos : pos_of(*mm->obj), static_hint(*so, mm->name));
                        exprs(c.args);
                        return;
                    }
                if (fn)
                    check_member(fn->access, *owner, mm->name, mm->name_pos ? mm->name_pos : pos_of(*mm->obj), d);
                if (fn)
                {
                    const uint32_t pos = pos_of(*mm->obj);
                    const std::string name = fn_symbol(*owner, *fn);
                    expr(mm->obj);
                    ExprP recv = raw(shell_text(*d, print(*mm->obj)), pos);
                    c.callee = raw(name, pos);
                    c.args.insert(c.args.begin(), std::move(recv));
                    coerce_args(c, fn, 1);
                    for (size_t i = 1; i < c.args.size(); ++i)
                        expr(c.args[i]);
                    return;
                }
            }
        }
        // A handle that goes to a native parameter "T*" or "T&": the call-site conversion
        // (task 33). The types are from before the lowering of the arguments.
        // "xs.push_back(c)" of a container of interface values: the class handle converts.
        if (auto* pm = std::get_if<MemberExpr>(&c.callee->v);
            pm && c.args.size() == 1 && (pm->name == "push_back" || pm->name == "push_front")
            && !std::holds_alternative<ThisExpr>(pm->obj->v) && !std::holds_alternative<BaseExpr>(pm->obj->v))
            if (const std::string el = element_type(type_of(*pm->obj), true); iface_named(el))
                coerce(c.args[0], el);
        const std::vector<char> forms = native_arg_forms(c);
        expr(c.callee);
        exprs(c.args);
        for (size_t i = 0; i < forms.size(); ++i)
        {
            if (!forms[i])
                continue;
            const uint32_t pos = pos_of(*c.args[i]);
            CallExpr w;
            w.pos = pos;
            w.callee = raw(forms[i] == 'p' ? "hot::as_ptr" : "hot::as_ref", pos);
            w.args.push_back(std::move(c.args[i]));
            c.args[i] = make_expr(std::move(w));
        }
    }

    // For each argument of the native call: 'p' (hot::as_ptr), 'r' (hot::as_ref) or 0 (keep). An
    // argument converts when its script type is a handle type T, the form of the parameter is
    // "T*" or "T&" and all overloads of the native function agree (hook "native_carrier").
    // Only an argument with a known type that is a name or a member access converts.
    std::vector<char> native_arg_forms(const CallExpr& c) const
    {
        std::vector<char> forms(c.args.size(), 0);
        if (!cx.native_key || !cx.native_carrier || !cx.types)
            return forms;
        const std::string key = cx.native_key(c);
        if (key.empty())
            return forms;
        for (size_t i = 0; i < c.args.size(); ++i)
        {
            const Expr* a = c.args[i].get();
            if (!a || !(std::holds_alternative<NameExpr>(a->v) || std::holds_alternative<MemberExpr>(a->v)))
                continue;
            const std::string t = type_of(*a);
            if (t.empty())
                continue;
            const std::string r = cx.native_carrier(key, i, c.args.size());
            if (r.size() > 2 && r[1] == ':' && (r[0] == 'p' || r[0] == 'r') && r.compare(2, std::string::npos, t) == 0
                && cx.types->is_handle(t))
                forms[i] = r[0];
        }
        return forms;
    }

    // True when "base.name(args)" with this number of arguments can only be a pure native method.
    // The pass has no argument types. A pure F(int) next to a concrete F(string) passes here and traps at run time.
    bool abstract_base_call(const std::string& name, size_t argc) const
    {
        const auto a = cx.abstract_natives.find(name);
        if (a == cx.abstract_natives.end() || !a->second.count(argc))
            return false;
        const auto c = cx.concrete_natives.find(name);
        return c == cx.concrete_natives.end() || !c->second.count(argc);
    }

    // "new C(args)" of a script class. The constructors that take this number of arguments are
    // the candidates (the pass has no argument types). If all candidates are private or
    // protected and the code cannot use them, this is an error. A class with no constructor has
    // a public default constructor.
    void check_ctor_access(const NewExpr& n)
    {
        const ClassDef* d = class_named(n.type);
        if (!d || d->ctors.empty())
            return;
        bool any = false, usable = false;
        Access first = Access::Public;
        for (const Ctor& c : d->ctors)
        {
            size_t required = 0;
            for (const Param& p : c.params)
                if (!p.def)
                    ++required;
            if (n.args.size() < required || n.args.size() > c.params.size())
                continue;
            if (!any)
                first = c.access;
            any = true;
            usable = usable || can_access(c.access, *d);
        }
        if (any && !usable)
            check_access(first, *d, "the constructor", n.pos);
    }

    // new T(args). The type model gives the callee: a handle, a value or a raw type.
    void new_expr(ExprP& e, NewExpr& n)
    {
        const uint32_t pos = n.pos;
        const bool handle_new = n.type.compare(0, 1, "$") == 0; // "new $C(...)"
        drop_handle_mark(n.type);
        const std::string type = n.type; // "n" is gone after "e" gets a new node
        if (cx.script_interfaces.count(n.type))
        {
            error(pos, "cannot create an instance of the interface '" + n.type + "'");
            return;
        }
        if (cx.script_classes.count(n.type))
        {
            if (!cx.new_classes.count(n.type))
            {
                error(pos, "class '" + n.type + "' must come before the class that builds it with 'new'");
                return;
            }
            if (const ClassDef* ad = class_named(n.type); ad && ad->is_abstract)
            {
                error(pos, "cannot create an instance of the abstract class '" + n.type + "'");
                return;
            }
            check_ctor_access(n);
            const ClassDef* script_def = class_named(type);
            if (!handle_new && script_def && !script_def->is_value && cx.types
                && !cx.types->script_native_base(type).empty())
            {
                error(pos, kNativeBaseValueError);
                return;
            }
            CallExpr c;
            c.pos = pos;
            c.args = std::move(n.args);
            // One constructor takes this number of arguments: its parameter types direct the
            // arguments. A "null" for a "[weak] T" parameter becomes an empty weak_ptr.
            if (const ClassDef* d = class_named(type))
            {
                const Ctor* only = nullptr;
                size_t found = 0;
                for (const Ctor& k : d->ctors)
                {
                    size_t required = 0;
                    for (const Param& p : k.params)
                        if (!p.def)
                            ++required;
                    if (c.args.size() >= required && c.args.size() <= k.params.size())
                        only = &k, ++found;
                }
                if (found == 1)
                    coerce_args(c, only->params, 0);
            }
            exprs(c.args);
            if (cx.types && cx.types->is_script_value(n.type))
            {
                // A struct: "new Stats(args)" is the value "Stats(args)" (design 7.10.1).
                c.callee = raw(n.type, pos);
                e = make_expr(std::move(c));
                return;
            }
            // A script class is a handle (design 7.10.1): "H(Name__new(args))". A std::shared_ptr
            // handle uses "H(Name__new_shared(args))": one block for the shell and the control block.
            // make_shared_shell gives a std::shared_ptr. So the shared pointer of another standard
            // namespace (the module attribute "std") uses "H(Name__new(args))".
            const std::string h = cx.types ? cx.types->cpp_storage("$" + type, TypeModel::StorageUse::Local) : type;
            const bool box = !handle_new && cx.types;
            const bool shared = h.rfind("std::shared_ptr<", 0) == 0
                || (h.rfind("\x01std::shared_ptr<", 0) == 0 && (!cx.types || cx.types->std_ns() == "std"));
            c.callee = raw(n.type + (shared ? "__new_shared" : "__new"), pos);
            e = make_expr(std::move(c));
            if (cx.types)
            {
                if (h != type)
                {
                    CallExpr w;
                    w.pos = pos;
                    w.callee = raw(h, pos);
                    w.args.push_back(std::move(e));
                    e = make_expr(std::move(w));
                }
            }
            if (box)
            {
                // "new C(args)" is a box: the new object goes into it without a copy.
                const std::string elem = value_elem(type);
                if (elem.empty())
                {
                    error(pos, "a script class value needs a std::shared_ptr handle, and '" + type + "' has not");
                    return;
                }
                e = call_of("hot::Value<" + elem + ">", args_of(std::move(e)), pos);
            }
            return;
        }
        exprs(n.args);
        TypeModel::Callee callee{n.type, false};
        if (cx.types)
            callee = cx.types->make_callee(n.type);
        CallExpr c;
        c.pos = pos;
        if (!callee.wrap_new)
        {
            c.callee = raw(callee.text, pos);
            c.args = std::move(n.args);
        }
        else
        {
            CallExpr inner;
            inner.pos = pos;
            inner.callee = raw("new " + n.type, pos);
            inner.args = std::move(n.args);
            c.callee = raw(callee.text, pos);
            c.args.push_back(make_expr(std::move(inner)));
        }
        e = make_expr(std::move(c));
    }

    // Wraps an object in "hot::deref(...)". The helper gives a reference for a pointer and
    // for a reference. So the member access after it is the same for both.
    void deref(ExprP& obj)
    {
        const uint32_t pos = pos_of(*obj);
        CallExpr c;
        c.pos = pos;
        c.callee = raw("hot::deref", pos);
        c.args.push_back(std::move(obj));
        obj = make_expr(std::move(c));
    }

    void member(ExprP& e, MemberExpr& m)
    {
        const bool target = std::exchange(weak_target, false);
        const uint32_t obj_pos = pos_of(*m.obj);
        if (const InterfaceDef* idef = iface_named(type_of(*m.obj)))
        {
            error(m.name_pos ? m.name_pos : obj_pos,
                "'" + m.name + "' is not a call of a method of the interface '" + idef->name
                    + "': an interface has methods only");
            return;
        }
        if (cx.in_default && (std::holds_alternative<ThisExpr>(m.obj->v) || std::holds_alternative<BaseExpr>(m.obj->v)))
        {
            error(obj_pos, kDefaultBan);
            return;
        }
        if (cx.in_static && std::holds_alternative<BaseExpr>(m.obj->v))
        {
            error(obj_pos, cx.in_free_fn ? "'base' is not allowed in a free function"
                                         : "'base' is not allowed in a static method");
            return;
        }
        if (std::holds_alternative<ThisExpr>(m.obj->v))
        {
            const ClassDef* prop_owner = nullptr;
            if (cx.in_static)
            {
                no_this_error(obj_pos);
            }
            else if (cx.cls && find_prop(*cx.cls, m.name, &prop_owner))
            {
                e = prop_read({m.name, nullptr, prop_owner}, obj_pos); // "m" is gone now
            }
            else if (!cx.allow_this)
            {
                error(obj_pos, "'this' is not allowed in a field initializer");
            }
            else if (cx.field_type.count(m.name))
            {
                const ClassDef* owner = nullptr;
                if (const Field* f = find_field(*cx.cls, m.name, &owner))
                    check_member(f->access, *owner, m.name, m.name_pos ? m.name_pos : obj_pos);
                const bool weak = !target && weak_field_of(*cx.cls, m.name);
                e = raw_named("__st->", m.name, obj_pos, m.name_pos); // "m" is gone now
                if (weak)
                    lock(e);
            }
            else if (cx.fns.count(m.name))
            {
                error(obj_pos, "script function '" + m.name + "' must be called");
                e = raw_named("", cx.cls->name + "__" + m.name, obj_pos, m.name_pos);
            }
            else
            {
                e = raw_named("__self->", m.name, obj_pos, m.name_pos);
            }
            return;
        }
        if (std::holds_alternative<BaseExpr>(m.obj->v))
        {
            if (cx.cls->base.empty())
            {
                error(obj_pos, "'base' needs a base class: '" + cx.cls->name + "' has none");
                return;
            }
            if (cx.abstract_natives.count(m.name) && !cx.concrete_natives.count(m.name))
            {
                error(obj_pos, "cannot call the abstract method '" + m.name + "' through 'base'");
                return;
            }
            // With a script parent, the member is a member of the native base of the root.
            if (parent_of(*cx.cls))
            {
                if (cx.native_base.empty())
                {
                    error(obj_pos,
                        "'base." + m.name + "' needs a native base: the classes of '" + cx.cls->name + "' have none");
                    return;
                }
                auto iv = cx.interface_virtuals.find(m.name);
                e = raw("__self->" + (iv == cx.interface_virtuals.end() ? cx.native_base : iv->second) + "::" + m.name,
                    obj_pos);
                return;
            }
            auto iv = cx.interface_virtuals.find(m.name);
            e = raw(
                "__self->" + (iv == cx.interface_virtuals.end() ? cx.cls->base : iv->second) + "::" + m.name, obj_pos);
            return;
        }
        // "obj.P", "obj.f": the type of obj decides. Without a type, P is a property of the
        // current class (the old rule).
        const std::string t = type_of(*m.obj);
        const ClassDef* td = class_named(t);
        const ClassDef* d = td ? td : prop_class(t); // nullptr for a known native type
        const ClassDef* prop_owner = nullptr;
        if (d && find_prop(*d, m.name, &prop_owner))
        {
            e = prop_read({m.name, &m.obj, prop_owner, d}, obj_pos); // "m" is gone now
            return;
        }
        if (td)
        {
            const ClassDef* owner = nullptr;
            if (const Field* f = find_field(*td, m.name, &owner))
                check_member(f->access, *owner, m.name, m.name_pos ? m.name_pos : obj_pos, td);
        }
        if (td && !td->is_value)
        {
            if (find_field(*d, m.name))
            { // a field of a script object behind a handle
                const bool weak = weak_field_of(*d, m.name);
                expr(m.obj);
                e = raw(cx.aot_optimized ? shell_text(*d, print(*m.obj)) + "->" + m.name
                                         : "state_of(" + shell_text(*d, print(*m.obj)) + ")->" + m.name,
                    obj_pos);
                if (weak && !target)
                    lock(e);
                return;
            }
            if (find_fn(*d, m.name))
            {
                error(obj_pos, "script function '" + m.name + "' must be called");
                return;
            }
            if (const ClassDef* so = nullptr; find_static(*d, m.name, &so))
            {
                error(m.name_pos ? m.name_pos : obj_pos, static_hint(*so, m.name));
                return;
            }
        }
        // A "[weak]" field of a script struct: "hot::lock(hot::deref(s).w)".
        const bool weak_value = td && td->is_value && weak_field_of(*td, m.name) && !target;
        expr(m.obj);
        deref(m.obj); // an empty std::optional throws hot::NullReference, as a null handle does
        if (weak_value)
            lock(e);
    }

    // True while the pass lowers the body of a lambda.
    bool in_lambda = false;

    // The state of the places where C++ does not allow "co_await". A catch handler is one. The
    // body of a finally block and the right side of "??" are lambdas in the C++ code.
    int catch_depth = 0;
    int lazy_depth = 0;
    void check_await(uint32_t pos)
    {
        if (catch_depth > 0)
            error(pos, "'await' is not supported inside a 'catch' block yet");
        else if (fin.inside)
            error(pos, "'await' is not supported inside a 'finally' block yet");
        else if (lazy_depth > 0)
            error(pos, "'await' cannot be used inside this expression yet");
    }

    void lambda(ExprP& e, LambdaExpr& l)
    {
        if (!cx.allow_this && !cx.in_static)
        {
            error(l.pos, "a lambda is not allowed in a field initializer");
            return;
        }
        if (cx.cls && cx.cls->is_value)
        {
            error(l.pos, "a lambda is not allowed in a struct: a value can move");
            return;
        }
        const ScopeSave scope(cx); // a parameter can hide a "[weak]" name of the function
        for (const Param& p : l.params)
        {
            // An empty type is "auto". Only a parameter without a name is an error.
            if (p.name.empty())
                error(l.pos, "a lambda parameter needs a name");
            declare(p.name, p.type);
            if (p.value)
                cx.value_locals.insert(p.name); // a bare script class parameter holds a box
        }
        if (cx.types)
            for (Param& p : l.params)
                if (!p.type.empty())
                    p.type = cpp_type(p.type, TypeModel::StorageUse::LambdaParam, l.pos);
        // A "return" in a lambda leaves only the lambda. So it is correct in a finally block.
        const FinallyState outer = fin;
        const std::string outer_ret = std::exchange(cx.ret_type, std::string());
        const bool outer_ret_value = std::exchange(cx.ret_value, false);
        fin = {};
        const bool outer_lambda = std::exchange(in_lambda, true);
        block(l.body);
        in_lambda = outer_lambda;
        std::set<std::string> param_names;
        for (const Param& p : l.params)
            param_names.insert(p.name);
        l.captures = binding_captures(bindings, text_of(l.body), false, param_names);
        fin = outer;
        cx.ret_type = outer_ret;
        cx.ret_value = outer_ret_value;
        // The lambda captures by value. "self" is a pointer to the shell. The state pointer
        // is read again inside, so a lambda never keeps the pointer to a block.
        l.body.stmts.insert(
            l.body.stmts.begin(), make_stmt(RawStmt{cx.aot_optimized ? "HOT_NODEBUG_LOCAL auto* __st = __self;"
                                           : "HOT_NODEBUG_LOCAL auto* __st = state_of(__self);",
                0}));
        const uint32_t pos = l.pos;
        CallExpr c;
        c.pos = pos;
        c.callee = raw("hot::make_closure", pos);
        c.args.push_back(raw("GEN", 0));
        c.args.push_back(raw("&hot_live", 0));
        c.args.push_back(raw("\"" + cx.where + " lambda\"", 0));
        c.args.push_back(std::move(e));
        e = make_expr(std::move(c));
    }

    // The template arguments that are expressions get the same lowering as other expressions.
    // The text changes only when the lowering changes an argument. So a type keeps its text.
    // The parser already wrote the type text with "::" for each scope.
    void targs(NameExpr& n)
    {
        if (!n.targ_parsed)
            return;
        auto joined = [&]
        {
            std::string r;
            for (const ExprP& a : n.targ_exprs)
                r += (r.empty() ? "" : ", ") + print(*a);
            return r;
        };
        const std::string before = joined();
        exprs(n.targ_exprs);
        const std::string after = joined();
        if (after != before)
            n.template_args = "<" + canonical_type(after) + ">";
    }

    // A script class in the template arguments of a type name is a handle (task 48): the text
    // "std.vector<Child>" becomes "std::vector<H>", where H is the storage of Child. A function
    // template that takes a type (a cast, "make_shared") keeps its argument.
    void handle_targs(NameExpr& n)
    {
        if (!cx.types || n.template_args.empty() || n.parts.empty())
            return;
        static const std::set<std::string> kKeep = {"make_shared", "make_unique", "allocate_shared",
            "static_pointer_cast", "dynamic_pointer_cast", "const_pointer_cast", "reinterpret_pointer_cast",
            "static_cast", "dynamic_cast", "const_cast", "reinterpret_cast", "sizeof", "alignof", "typeid", "declval",
            "as", "is", "is_same", "is_same_v"};
        const std::string& last = n.parts.back();
        if (kKeep.count(last) || n.parts[0] == "hot")
            return;
        n.template_args = cpp_type(last + n.template_args, TypeModel::StorageUse::Local, n.pos).substr(last.size());
    }

    // A bare name. A property of the class becomes a call of the getter.
    // True when the name is a module variable that no local hides.
    bool is_module_var(const std::string& n) const { return cx.module_vars.count(n) != 0 && cx.locals.count(n) == 0; }

    // True when a bare name is a member of the native base of the class. A local, a field, a
    // property and a script fn of the class (or of a script parent) hide it.
    bool native_member_name(const std::string& n) const
    {
        if (!cx.cls || cx.cls->is_value || !cx.native_members.count(n))
            return false;
        if (cx.locals.count(n) || cx.field_type.count(n) || cx.fns.count(n) || cx.props.count(n))
            return false;
        return !find_field(*cx.cls, n) && !find_prop(*cx.cls, n) && !find_fn(*cx.cls, n);
    }

    bool is_native_member(const std::string& n) const { return cx.allow_this && native_member_name(n); }

    // True when a bare name is a script member of "this": a field or a property of the class or
    // of a script parent. A local hides it. It wins over a module variable.
    bool is_script_member(const std::string& n) const
    {
        if (!cx.cls || cx.locals.count(n))
            return false;
        // The class of the module variables has them as fields. Here they are module variables.
        if (cx.in_module_init || cx.in_module_hook)
            return false;
        return cx.field_type.count(n) || find_field(*cx.cls, n) || find_prop(*cx.cls, n);
    }

    // True when a bare name is a member of "this": a script member, or a member of the native
    // base. A local hides it. A module variable wins over a native member only. This is true
    // also in a field initializer, where the use of "this" is an error.
    bool is_this_member(const std::string& n) const
    {
        if (is_script_member(n))
            return true;
        return !is_module_var(n) && native_member_name(n);
    }

    // The access to a module variable: "hot_mod()->name".
    ExprP module_ref(const std::string& n, uint32_t pos)
    {
        if (cx.in_module_init && !cx.module_earlier.count(n))
            error(pos, "the module variable '" + n +
                           "' is not declared before this variable. An initializer of a module variable can use only "
                           "the module variables declared before it");
        if (cx.in_module_hook)
            error(pos,
                "the module variable '" + n
                    + "' cannot be used in a convert hook: the new module state is not complete yet");
        return raw("hot_mod()->" + n, pos);
    }

    static constexpr const char* kDefaultBan = "a default value cannot use a field, a parameter or 'this'";

    // In a default value: true when the first part of the name is a script member or a parameter.
    bool default_banned(const std::string& head) const
    {
        if (!cx.in_default)
            return false;
        if (cx.locals.count(head) || is_module_var(head) || cx.fns.count(head) || cx.field_type.count(head))
            return true;
        // A member of the native base (a static member too) is a member of the shell.
        if (cx.native_members.count(head))
            return true;
        return cx.cls && (find_field(*cx.cls, head) || find_prop(*cx.cls, head));
    }

    void name(ExprP& e, NameExpr& n)
    {
        const bool target = std::exchange(weak_target, false);
        // A read of a "[weak]" name locks it. The node of the name can change below, so the
        // flag comes first.
        const bool weak = !target && n.parts.size() == 1 && n.template_args.empty() && weak_bare(n.parts[0]);
        name_inner(e, n);
        if (weak && e)
            lock(e);
    }

    void name_inner(ExprP& e, NameExpr& n)
    {
        if (!n.parts.empty() && default_banned(n.parts[0]))
        {
            error(n.pos, kDefaultBan);
            return;
        }
        targs(n);
        handle_targs(n);
        const bool bare = n.parts.size() == 1 && !cx.locals.count(n.parts[0]);
        // A function name that is not a call. A script function has no address (a reload replaces it).
        if ((bare && !is_this_member(n.parts[0]) && !is_module_var(n.parts[0]) && bare_function(n.parts[0]))
            || scope_static(n))
        {
            error(n.pos, "'" + n.parts.back() + "' is a function; call it, or write a lambda");
            return;
        }
        // A script member wins over a module variable. A module variable wins over a native member.
        if (bare && is_module_var(n.parts[0]) && !is_script_member(n.parts[0]))
        {
            e = module_ref(n.parts[0], n.pos);
            return;
        }
        if (std::optional<PropRef> r = prop_ref(*e))
        {
            e = prop_read(std::move(*r), n.pos);
            return;
        }
        // A bare member in a field initializer: there is no "this".
        if (bare && !cx.allow_this && !cx.in_default && is_this_member(n.parts[0]))
        {
            if (cx.in_static)
                needs_object_error(n.parts[0], n.pos);
            else
                error(n.pos, "'this' is not allowed in a field initializer");
            return;
        }
        if (bare)
        {
            // A bare field of a struct: its functions are free functions, "__st" is the object.
            // In a lambda, a field of a class reads "__st" too: the lambda must not capture "this".
            if (cx.cls && cx.allow_this && (cx.cls->is_value || in_lambda)
                && (cx.field_type.count(n.parts[0]) || find_field(*cx.cls, n.parts[0])))
            {
                e = raw_named("__st->", n.parts[0], n.pos, n.pos);
                return;
            }
            // A bare member of the native base: the member of the shell.
            if (is_native_member(n.parts[0]))
            {
                e = raw_named("__self->", n.parts[0], n.pos, n.pos);
                return;
            }
        }
        // A bare name of a field of a script parent: a private field of the parent is an error.
        if (cx.cls && cx.allow_this && !n.parts.empty() && !cx.locals.count(n.parts[0]))
        {
            const ClassDef* owner = nullptr;
            if (const Field* f = find_field(*cx.cls, n.parts[0], &owner))
                check_member(f->access, *owner, n.parts[0], n.pos);
        }
    }

    // A name with more than one part. If the first part is a local, the name is a member
    // chain: "a.b.c" becomes the node of "a.b.c" with MemberExpr. The lowering of it is
    // "hot::deref(hot::deref(a).b).c". Else it is a scope chain: the printer joins the
    // parts with "::". The change comes before the property rewrite, so "p.P" is "obj.P".
    void member_chain(ExprP& e)
    {
        auto* np = std::get_if<NameExpr>(&e->v);
        if (!np || np->parts.size() < 2)
            return;
        // A member of "this" is the head of "m.x": a field or a property of the class, or a
        // member of the native base. The member wins over a type or a namespace of that name.
        // A script member wins over a module variable. A module variable wins over a native member.
        const bool module_head = is_module_var(np->parts[0]) && !is_script_member(np->parts[0]);
        const bool this_head = !module_head && !cx.locals.count(np->parts[0]) && is_this_member(np->parts[0]);
        if (!module_head && !this_head && !cx.locals.count(np->parts[0]))
            return;
        NameExpr& n = *np;
        targs(n);
        const uint32_t pos = n.pos;
        NameExpr head;
        head.parts = {n.parts[0]};
        head.pos = pos;
        ExprP chain = module_head ? module_ref(n.parts[0], pos) : make_expr(std::move(head));
        if (this_head)
            chain = make_expr(MemberExpr{make_expr(ThisExpr{pos}), n.parts[0], pos, pos});
        for (size_t i = 1; i < n.parts.size(); ++i)
        {
            std::string member_name = n.parts[i];
            if (i + 1 == n.parts.size())
                member_name += n.template_args;
            const uint32_t name_pos = i < n.part_pos.size() ? n.part_pos[i] : pos;
            chain = make_expr(MemberExpr{std::move(chain), std::move(member_name), pos, name_pos});
        }
        e = std::move(chain); // "n" is gone now
    }

    // True for the next expr() call when the value of the expression is not used: the
    // expression of a statement and a step of a for loop.
    bool discard_next = false;

    void expr(ExprP& e)
    {
        const bool discard = std::exchange(discard_next, false);
        const bool target = std::exchange(target_next, false);
        if (!e)
            return;
        member_chain(e);
        if (prop_write(e, discard))
            return;
        if (safe_expr(e))
            return;
        Expr& x = *e;
        if (auto* n = std::get_if<ThisExpr>(&x.v))
        {
            if (cx.in_default)
                error(n->pos, kDefaultBan);
            else if (!cx.allow_this)
                no_this_error(n->pos);
            else
                e = raw("__self", n->pos);
        }
        else if (auto* n2 = std::get_if<NullExpr>(&x.v))
        {
            e = raw("nullptr", n2->pos);
        }
        else if (auto* nm = std::get_if<NameExpr>(&x.v))
        {
            weak_target = target;
            name(e, *nm);
        }
        else if (auto* num = std::get_if<NumberExpr>(&x.v))
        {
            num->text = rewrite_number(num->text);
        }
        else if (auto* m = std::get_if<MemberExpr>(&x.v))
        {
            weak_target = target;
            member(e, *m);
        }
        else if (auto* c = std::get_if<CallExpr>(&x.v))
        {
            call(*c);
        }
        else if (auto* ix = std::get_if<IndexExpr>(&x.v))
        {
            if (ix->obj && op_on_handle(*ix->obj, "[]", 1, pos_of(*ix->obj)))
                return;
            expr(ix->obj);
            expr(ix->index);
        }
        else if (auto* u = std::get_if<UnaryExpr>(&x.v))
        {
            if (!u->postfix && u->op != "!" && u->operand
                && op_on_handle(*u->operand, u->op, 0, u->pos != 0 ? u->pos : pos_of(*u->operand)))
                return;
            if ((u->op == "++" || u->op == "--") && u->operand
                && op_index_target_error(*u->operand, u->pos != 0 ? u->pos : pos_of(*u->operand)))
                return;
            if (u->op == "&" && !u->postfix && u->operand)
                if (const std::string w = weak_root(*u->operand); !w.empty())
                    weak_ref_error(u->pos ? u->pos : pos_of(*u->operand), w);
            if (u->op == "await" && !u->postfix)
            {
                check_await(u->pos);
                if (!in_lambda)
                    ++cx.co_keywords;
            }
            if (!u->postfix && cx.types && u->op.size() > 3 && u->op[0] == '(' && u->op[1] == '$' && u->op.back() == ')')
            { // "($C)h": the cast to the handle of "C", written with the C++ type of the handle
                const std::string h = cpp_type(u->op.substr(1, u->op.size() - 2), TypeModel::StorageUse::Local, u->pos);
                const std::string to = u->op.substr(2, u->op.size() - 3);
                std::string from = u->operand ? object_type(type_of(*u->operand)) : std::string();
                if (!from.empty() && from[0] == '$')
                    from.erase(0, 1);
                // An upcast and a cast to the same type cannot fail: "static_pointer_cast". Every other
                // cast is checked with the class record, as "as" is: a failed cast gives a null handle.
                // A cast to the same type or to a base type is a checked unwrap (hot::nonnull): a null
                // handle throws hot::NullReference and the result is not null.
                bool upcast = false;
                for (const ClassDef* d = class_named(from); d && !upcast; d = class_named(d->base))
                    upcast = d->name == to;
                const bool same = !from.empty() && from == to;
                const bool checked = !upcast && !same && class_named(to) != nullptr;
                expr(u->operand);
                u->op = "(" + h + ")";
                for (const char* sp : {"\x01std::shared_ptr<", "std::shared_ptr<"})
                    if (h.rfind(sp, 0) == 0 && h.back() == '>')
                    { // a shared handle
                        CallExpr c;
                        c.pos = u->pos;
                        if (checked)
                        {
                            c.callee = raw("hot::as_script<" + to + "_Shell>", u->pos);
                            c.args.push_back(std::move(u->operand));
                            c.args.push_back(raw("\"" + to + "\"", u->pos));
                        }
                        else if (same || upcast)
                        {
                            // The cast of an upcast keeps the handle conversion, after the null test.
                            c.callee = raw("hot::nonnull", u->pos);
                            c.args.push_back(std::move(u->operand));
                            if (!same)
                            {
                                CallExpr conv;
                                conv.pos = u->pos;
                                conv.callee = raw(std::string(sp, std::strlen(sp) - std::strlen("shared_ptr<")) + "static_pointer_cast<"
                                        + h.substr(std::strlen(sp), h.size() - std::strlen(sp) - 1) + ">", u->pos);
                                conv.args.push_back(make_expr(std::move(c)));
                                c = std::move(conv);
                            }
                        }
                        else
                        {
                            c.callee = raw(std::string(sp, std::strlen(sp) - std::strlen("shared_ptr<")) + "static_pointer_cast<"
                                    + h.substr(std::strlen(sp), h.size() - std::strlen(sp) - 1) + ">", u->pos);
                            c.args.push_back(std::move(u->operand));
                        }
                        e = make_expr(std::move(c));
                        return;
                    }
                // Another handle (an intrusive pointer, or a raw pointer): the same checks, then the C cast.
                if (checked || same || upcast)
                {
                    CallExpr c;
                    c.pos = u->pos;
                    if (checked)
                    {
                        c.callee = raw("hot::as_script<" + to + "_Shell>", u->pos);
                        c.args.push_back(std::move(u->operand));
                        c.args.push_back(raw("\"" + to + "\"", u->pos));
                    }
                    else
                    {
                        c.callee = raw("hot::nonnull", u->pos);
                        c.args.push_back(std::move(u->operand));
                    }
                    u->operand = make_expr(std::move(c));
                }
                return;
            }
            if (!u->postfix && is_ptr_cast(u->op) && !is_address_of(u->operand))
            { // "(T*)p": a checked unwrap when "p" has the type "T*" (the C++ compiler decides), else the C cast
                expr(u->operand);
                CallExpr c;
                c.pos = u->pos;
                c.callee = raw("hot::cast_ptr<" + u->op.substr(1, u->op.size() - 2) + ">", u->pos);
                c.args.push_back(std::move(u->operand));
                e = make_expr(std::move(c));
                return;
            }
            expr(u->operand);
        }
        else if (auto* b = std::get_if<BinaryExpr>(&x.v))
        {
            if (b->op == "=" && b->lhs && b->rhs)
            {
                if (is_value_ref(*b->lhs))
                    value_wrap(b->rhs, type_of(*b->lhs)); // the object is copied into the box
                else
                {
                    coerce(b->rhs, type_of(*b->lhs));
                    value_not_to_handle(b->rhs, type_of(*b->lhs));
                }
            }
            if ((b->op == "==" || b->op == "!=") && b->lhs && b->rhs)
            { // "x == null" and "x != null" on a "T?" compare with the empty optional
                if (std::holds_alternative<NullExpr>(b->rhs->v) && is_optional(type_of(*b->lhs)))
                    coerce(b->rhs, type_of(*b->lhs));
                else if (std::holds_alternative<NullExpr>(b->lhs->v) && is_optional(type_of(*b->rhs)))
                    coerce(b->lhs, type_of(*b->rhs));
            }
            if (b->lhs && b->rhs && op_on_handle(*b->lhs, b->op, 1, b->pos != 0 ? b->pos : pos_of(*b->lhs)))
                return;
            if (b->lhs && b->rhs && is_assign_op(b->op) && op_index_target_error(*b->lhs, b->pos != 0 ? b->pos : pos_of(*b->lhs)))
                return;
            if (b->lhs && b->rhs && op_compound_error(*b, b->pos != 0 ? b->pos : pos_of(*b->lhs)))
                return;
            if (b->lhs && b->rhs && op_rhs_error(x, *b, b->pos != 0 ? b->pos : pos_of(*b->lhs)))
                return;
            if ((b->op == "==" || b->op == "!=") && b->lhs && b->rhs
                && (is_value_ref(*b->lhs) || is_value_ref(*b->rhs))
                && !(op_use(x) && op_rhs_problem(*op_use(x), *b->rhs).empty()))
            {
                error(b->pos != 0 ? b->pos : pos_of(*b->lhs),
                    "'" + b->op + "' is not valid for a value of a script class: it is never null and it has no identity");
                return;
            }
            if ((b->op == "==" || b->op == "!=") && b->lhs && b->rhs
                && (iface_named(type_of(*b->lhs)) || iface_named(type_of(*b->rhs))))
            {
                iface_compare(e, *b);
                return;
            }
            if (is_assign_op(b->op) && b->lhs && safe_target(*b->lhs))
            {
                error(pos_of(*b->lhs), "'?.' cannot be the target of an assignment inside an expression");
                return;
            }
            if (b->op == "??" && b->lhs && b->rhs && std::holds_alternative<NullExpr>(b->rhs->v)
                && is_optional(type_of(*b->lhs)))
            { // "x ?? null" on a "T?" is "x": the empty optional adds nothing
                ExprP lhs = std::move(b->lhs);
                expr(lhs);
                e = std::move(lhs);
                return;
            }
            // "w = e" writes the weak handle "w": the target is not locked. "w = null" resets it.
            if (b->op == "=" && b->lhs)
                member_chain(b->lhs);
            const bool div_op = (b->op == "/" || b->op == "%" || b->op == "/=" || b->op == "%=") && b->lhs && b->rhs;
            if (div_op)
            { // the kinds need the form that the lowering of the name chain gives
                member_chain(b->lhs);
                member_chain(b->rhs);
            }
            const bool use_div = div_op && div_checked(num_kind(*b->lhs), *b->rhs);
            const bool weak_lhs = b->op == "=" && b->lhs && is_weak_handle(*b->lhs);
            const bool reset = weak_lhs && b->rhs && std::holds_alternative<NullExpr>(b->rhs->v);
            if (weak_lhs)
                reject_weak_new(b->rhs);
            target_next = weak_lhs;
            expr(b->lhs);
            if (reset)
            {
                e = make_expr(UnaryExpr{".reset()", std::move(b->lhs), true, b->pos});
                return;
            }
            // The right side of "??" is in a lambda: it cannot hold an "await".
            if (b->op == "??")
                ++lazy_depth;
            expr(b->rhs);
            if (b->op == "??")
                --lazy_depth;
            // An integer "/" and "%" call the checked helper. A compound form passes the target by
            // reference, so the helper evaluates it one time.
            if (use_div)
            {
                const bool assign = b->op.back() == '=';
                const std::string name = std::string(b->op[0] == '/' ? "div" : "mod") + (assign ? "_assign" : "");
                const uint32_t pos = b->pos != 0 ? b->pos : pos_of(*b->lhs);
                e = call_of("hot::" + name, args_of(std::move(b->lhs), std::move(b->rhs)), pos);
                return;
            }
            // "x ?? y" is a call of the runtime helper. The helper reads x one time. The
            // second argument is a lambda, so y runs only when x is false.
            if (b->op == "??" && b->lhs && b->rhs)
            {
                const uint32_t pos = b->pos != 0 ? b->pos : pos_of(*b->lhs);
                ExprP inner = make_expr(ParenExpr{std::move(b->rhs), 0});
                // A default value is not in a function, so its lambda has no capture. The lambda gives a
                // value (not a reference), so a temporary on the right side cannot dangle.
                const std::string open = cx.in_default
                    ? std::string("[]() -> auto { return ")
                    : "[&" + binding_captures(bindings, print(*inner), true) + "]() -> auto { return ";
                ExprP head = make_expr(UnaryExpr{open, std::move(inner), false, 0});
                ExprP lazy = make_expr(UnaryExpr{"; }", std::move(head), true, 0});
                e = call_of("hot::coalesce", args_of(std::move(b->lhs), std::move(lazy)), pos);
                return;
            }
        }
        else if (auto* t = std::get_if<TernaryExpr>(&x.v))
        {
            expr(t->cond);
            expr(t->a);
            expr(t->b);
        }
        else if (auto* p = std::get_if<ParenExpr>(&x.v))
        {
            expr(p->inner);
        }
        else if (auto* nw = std::get_if<NewExpr>(&x.v))
        {
            new_expr(e, *nw);
        }
        else if (auto* l = std::get_if<LambdaExpr>(&x.v))
        {
            lambda(e, *l);
        }
        else if (auto* tt = std::get_if<TypeTestExpr>(&x.v))
        {
            type_test(e, *tt);
        }
        else if (auto* ip = std::get_if<InterpExpr>(&x.v))
        {
            interp(e, *ip);
        }
        else if (auto* co = std::get_if<CollectionExpr>(&x.v))
        {
            if (!typed_collections.count(&x))
                error(co->pos, "a collection literal needs a declared type");
            // A literal inside a typed literal gets its type from the C++ braces.
            for (ExprP& a : co->elems)
                nested_collection(a);
            for (ExprP& a : co->values)
                nested_collection(a);
            exprs(co->elems);
            exprs(co->values);
        }
    }

    void nested_collection(const ExprP& a)
    {
        if (a && std::holds_alternative<CollectionExpr>(a->v))
            typed_collections.insert(a.get());
    }

    // The underlying type of the script enum that "x" has, or an empty text.
    std::string enum_underlying(const Expr& x) const
    {
        // A conditional or parentheses: both branches must have the same enum underlying type.
        if (auto* p = std::get_if<ParenExpr>(&x.v))
            return p->inner ? enum_underlying(*p->inner) : std::string();
        if (auto* tn = std::get_if<TernaryExpr>(&x.v))
        {
            if (!tn->a || !tn->b)
                return {};
            const std::string ua = enum_underlying(*tn->a);
            return ua == enum_underlying(*tn->b) ? ua : std::string();
        }
        std::string t = type_of(x);
        if (t.empty())
            if (auto* nm = std::get_if<NameExpr>(&x.v))
                if (nm->parts.size() == 2 && !cx.locals.count(nm->parts[0]) && !cx.field_type.count(nm->parts[0]))
                    t = nm->parts[0]; // Mode.Walk
        auto it = cx.script_enums.find(t);
        return it == cx.script_enums.end() ? std::string() : it->second;
    }

    // A string with "${...}" (design 7.3, DECISIONS item 10) becomes a call of std::format or
    // fmt::format. The literal parts become the format string: "{" and "}" are doubled and the
    // escape "\$" gives "$". A string with no argument stays a plain literal.
    void interp(ExprP& e, InterpExpr& n)
    {
        const uint32_t pos = n.pos;
        for (ExprP& a : n.args)
        {
            // A script enum is an "enum class": the format library has no formatter for it. The
            // text shows the underlying integer, so the argument is cast to the underlying type.
            const std::string under = enum_underlying(*a);
            expr(a);
            if (!under.empty())
            {
                std::vector<ExprP> one;
                one.push_back(std::move(a));
                a = call_of("static_cast<" + under + ">", std::move(one), pos);
            }
        }
        const bool has_args = !n.args.empty();
        std::string f = "\"";
        for (size_t i = 0; i < n.lits.size(); ++i)
        {
            const std::string& t = n.lits[i];
            for (size_t k = 0; k < t.size(); ++k)
            {
                if (t[k] == '\\' && k + 1 < t.size())
                {
                    if (t[k + 1] != '$')
                        f += t[k];
                    f += t[k + 1];
                    ++k;
                }
                else if (has_args && (t[k] == '{' || t[k] == '}'))
                {
                    f += t[k];
                    f += t[k];
                }
                else
                {
                    f += t[k];
                }
            }
            if (i < n.args.size())
                f += n.specs[i].empty() ? "{}" : "{:" + n.specs[i] + "}";
        }
        f += "\"";
        if (!has_args)
        {
            e = raw(std::move(f), pos);
            return;
        }
        if (cx.uses_format)
            *cx.uses_format = true;
        CallExpr c;
        c.pos = pos;
        c.callee = raw(cx.format_lib == "fmt" ? "fmt::format" : "std::format", pos);
        c.args.push_back(raw(std::move(f), pos));
        for (ExprP& a : n.args)
            c.args.push_back(std::move(a));
        e = make_expr(std::move(c));
    }

    // "obj is T" and "obj as T" (design 7.7). A script class T uses the class records:
    // "hot::is_script(obj, "T")" and "hot::as_script<T_Shell>(obj, "T")". A native type T uses
    // RTTI: "hot::is_native<T>(obj)" and "hot::as_native<T>(obj)" (dynamic_cast).
    void type_test(ExprP& e, TypeTestExpr& t)
    {
        const uint32_t pos = t.pos;
        const bool bare = t.type.compare(0, 1, "$") != 0;
        drop_handle_mark(t.type);
        const std::string type = t.type;
        if (!t.bind.empty())
            error(t.bind_pos, "a 'is T name' binding is allowed only at the start of an if or while condition");
        const ClassDef* d = class_named(type);
        if (d && d->is_value)
        {
            error(pos,
                std::string("'") + (t.is_as ? "as" : "is") + "' needs a class with a handle: '" + type
                    + "' is a struct");
            return;
        }
        if (d && bare)
        { // a bare script class is a box: the test needs the handle
            const std::string kw = t.is_as ? "as" : "is";
            error(pos, "'" + kw + " " + type + "' names a value; write '" + kw + " $" + type + "'");
            return;
        }
        const std::string obj_type = object_type(type_of(*t.obj)); // before the lowering of the object
        const bool from_iface = iface_named(obj_type) != nullptr;
        expr(t.obj);
        CallExpr c;
        c.pos = pos;
        if (iface_named(type))
        { // "x is I" and "x as I" use the interface list of the class record
            c.callee = raw(t.is_as ? "hot::as_iface<" + type + "__iface>" : std::string("hot::is_iface"), pos);
            c.args.push_back(std::move(t.obj));
            c.args.push_back(raw("\"" + type + "\"", pos));
            e = make_expr(std::move(c));
            return;
        }
        if (d && from_iface && t.is_as && cx.types)
        { // "i as C": the class handle of the object behind the interface value
            c.callee = raw("hot::iface_as<" + cx.types->cpp_storage(handle_form(type), TypeModel::StorageUse::Local) + ", " + type + "_Shell>", pos);
            c.args.push_back(std::move(t.obj));
            c.args.push_back(raw("\"" + type + "\"", pos));
            e = make_expr(std::move(c));
            return;
        }
        if (d)
            c.callee = raw(t.is_as ? "hot::as_script<" + type + "_Shell>" : std::string("hot::is_script"), pos);
        else
            c.callee = raw((t.is_as ? "hot::as_native<" : "hot::is_native<") + type + ">", pos);
        c.args.push_back(std::move(t.obj));
        if (d)
            c.args.push_back(raw("\"" + type + "\"", pos));
        e = make_expr(std::move(c));
    }

    // "if (x is T g && rest)": the binding is the leftmost operand of the condition. The
    // lowering takes it out and gives the declaration "auto g = <x as T>;". The first operand
    // of the condition is then the name "g". This is a take of the first operand only.
    struct Binding
    {
        std::string name, type;
        uint32_t pos = 0, name_pos = 0;
        ExprP init;
    };
    static std::optional<Binding> take_binding(ExprP& cond)
    {
        ExprP* leaf = &cond;
        while (*leaf)
        {
            auto* b = std::get_if<BinaryExpr>(&(*leaf)->v);
            if (!b || b->op != "&&" || !b->lhs)
                break;
            leaf = &b->lhs;
        }
        auto* t = *leaf ? std::get_if<TypeTestExpr>(&(*leaf)->v) : nullptr;
        if (!t || t->is_as || t->bind.empty())
            return std::nullopt;
        Binding bd;
        bd.name = t->bind;
        bd.type = t->type;
        bd.pos = t->pos;
        bd.name_pos = t->bind_pos;
        bd.init = make_expr(TypeTestExpr{std::move(t->obj), t->type, true, t->pos});
        NameExpr nm;
        nm.parts.push_back(bd.name);
        nm.pos = nm.last_pos = t->bind_pos;
        *leaf = make_expr(std::move(nm)); // "t" is gone now
        return bd;
    }

    // True for an assignment operator ("=", "+=" and so on), but not for a comparison.
    static bool is_assign_op(const std::string& op)
    {
        return op == "="
            || (op.size() >= 2 && op.back() == '=' && op != "==" && op != "!=" && op != "<=" && op != ">=");
    }

    // The innermost "?." in the leftmost chain of the expression of a statement. The chain goes
    // down the callee of a call, the object of a member or an index, the operand of a postfix
    // operator and the left side of an assignment.
    static MemberExpr* find_safe(ExprP& root)
    {
        MemberExpr* found = nullptr;
        ExprP* cur = &root;
        while (cur && *cur)
        {
            ExprP* next = nullptr;
            Expr& x = **cur;
            if (auto* m = std::get_if<MemberExpr>(&x.v))
            {
                if (m->safe)
                    found = m;
                next = &m->obj;
            }
            else if (auto* c = std::get_if<CallExpr>(&x.v))
            {
                next = &c->callee;
            }
            else if (auto* ix = std::get_if<IndexExpr>(&x.v))
            {
                next = &ix->obj;
            }
            else if (auto* u = std::get_if<UnaryExpr>(&x.v))
            {
                if (u->postfix)
                    next = &u->operand;
            }
            else if (auto* b = std::get_if<BinaryExpr>(&x.v))
            {
                if (is_assign_op(b->op))
                    next = &b->lhs;
            }
            cur = next;
        }
        return found;
    }

    // True for "&x": the address of an object is never null, so a cast of it needs no unwrap.
    static bool is_address_of(const ExprP& e)
    {
        const auto* u = e ? std::get_if<UnaryExpr>(&e->v) : nullptr;
        return u && !u->postfix && u->op == "&";
    }

    // True for the operator text of a cast to a plain pointer type: "(T*)", not "(const T*)" and
    // not a handle, a reference or an optional.
    static bool is_ptr_cast(const std::string& op)
    {
        if (op.size() < 4 || op.front() != '(' || op.back() != ')' || op[op.size() - 2] != '*')
            return false;
        const std::string t = op.substr(1, op.size() - 2);
        if (t.find_first_of("$&?()[]") != std::string::npos || t.compare(0, 6, "const ") == 0
            || t.find(" const") != std::string::npos)
            return false;
        return true;
    }

    // True when the expression is "a?.b" or "a?.f(x)": a value that is not a place.
    static bool safe_target(const Expr& x)
    {
        const MemberExpr* m = std::get_if<MemberExpr>(&x.v);
        if (!m)
            if (const auto* c = std::get_if<CallExpr>(&x.v); c && c->callee)
                m = std::get_if<MemberExpr>(&c->callee->v);
        return m && m->safe;
    }

    // A new node for a name without template arguments.
    static ExprP copy_name(const NameExpr& n)
    {
        NameExpr c;
        c.parts = n.parts;
        c.pos = n.pos;
        c.last_pos = n.last_pos;
        return make_expr(std::move(c));
    }

    // "foreach (ref x in enumerate(xs))": the range gives temporary values, so a reference
    // cannot bind to them.
    void check_ref_range(const ForOfStmt& fo)
    {
        if (!fo.is_ref || !fo.range)
            return;
        const auto* call = std::get_if<CallExpr>(&fo.range->v);
        const auto* callee = call ? std::get_if<NameExpr>(&call->callee->v) : nullptr;
        if (!callee || !callee->template_args.empty())
            return;
        const bool bare = callee->parts.size() == 1 && is_helper_name(callee->parts[0]);
        const bool scoped = callee->parts.size() == 2 && callee->parts[0] == "hot";
        if (!bare && !scoped)
            return;
        const std::string& helper = callee->parts.back();
        if (helper != "enumerate" && helper != "range")
            return;
        std::string use = "var ";
        if (fo.names.empty())
        {
            use += fo.name;
        }
        else
        {
            use += "(";
            for (size_t i = 0; i < fo.names.size(); ++i)
                use += (i ? ", " : "") + fo.names[i];
            use += ")";
        }
        error(callee->pos, "'ref' cannot bind to the temporary value of '" + helper + "'; use '" + use + "'");
    }

    // "h?.f(a);" becomes "{ if (h) h.f(a); }". When the receiver is not a simple name, the
    // receiver goes in a temporary local, so it runs one time:
    // "{ auto __safeN = recv; if (__safeN) __safeN.f(a); }". The new statement is lowered here.
    // A second "?." in the chain is lowered by the new statement.
    bool safe_stmt(StmtP& s, ExprStmt& es)
    {
        MemberExpr* m = find_safe(es.expr);
        if (!m)
            return false;
        const uint32_t pos = m->pos;
        BlockStmt blk;
        blk.pos = es.pos;
        ExprP cond;
        const auto* nm = std::get_if<NameExpr>(&m->obj->v);
        // A property is a call of the getter, so it goes in a temporary too.
        const bool is_prop =
            nm && nm->parts.size() == 1 && !cx.locals.count(nm->parts[0]) && cx.cls && find_prop(*cx.cls, nm->parts[0]);
        if (nm && nm->parts.size() == 1 && nm->template_args.empty() && !is_prop)
        {
            cond = copy_name(*nm);
        }
        else
        {
            NameExpr tmp;
            tmp.parts.push_back("__safe" + std::to_string(safe_count++));
            tmp.pos = tmp.last_pos = pos;
            blk.stmts.push_back(
                make_stmt(VarDeclStmt{false, false, tmp.parts[0], std::string(), std::move(m->obj), pos, pos}));
            m->obj = copy_name(tmp);
            cond = copy_name(tmp);
        }
        m->safe = false;
        ExprStmt inner = std::move(es);
        const uint32_t spos = inner.pos;
        blk.stmts.push_back(make_stmt(IfStmt{std::move(cond), make_stmt(std::move(inner)), nullptr, spos, 0}));
        s->v = std::move(blk);
        stmt(s);
        return true;
    }
    int safe_count = 0;

    // "a?.b" and "a?.f(x)" inside an expression. The receiver "a" is the first argument of
    // "hot::safe_access". The second argument is a lambda that has the receiver as its parameter, so
    // "a" runs one time: "hot::safe_access(a, [&](auto&& __safe0) { return hot::deref(__safe0).b; })".
    // The lambda body is the member access (or the call) with the parameter as the receiver. The
    // result is the member type made nullable (see hot::safe_access). A "?." after the first one
    // is in the receiver, so it is lowered by the same function. Returns true when it did so.
    bool safe_expr(ExprP& e)
    {
        MemberExpr* m = std::get_if<MemberExpr>(&e->v);
        if (!m)
            if (auto* c = std::get_if<CallExpr>(&e->v); c && c->callee)
                m = std::get_if<MemberExpr>(&c->callee->v);
        if (!m || !m->safe || !m->obj)
            return false;
        const uint32_t pos = m->pos;
        std::string recv_type = type_of(*m->obj);
        if (!recv_type.empty() && recv_type.back() == '?')
            recv_type.pop_back();
        ExprP recv = std::move(m->obj);
        expr(recv);
        const std::string tmp = "__safe" + std::to_string(safe_count++);
        NameExpr tn;
        tn.parts.push_back(tmp);
        tn.pos = tn.last_pos = pos;
        m->obj = make_expr(std::move(tn));
        m->safe = false;
        {
            const ScopeSave scope(cx);
            declare(tmp, recv_type);
            ++lazy_depth; // the code is in a lambda: it cannot hold an "await"
            expr(e);
            --lazy_depth;
        }
        ExprP inner = make_expr(ParenExpr{std::move(e), 0});
        const std::string param = "(auto&& " + tmp + ") { return ";
        const std::string open = cx.in_default ? "[]" + param : "[&" + binding_captures(bindings, print(*inner), true) + "]" + param;
        ExprP head = make_expr(UnaryExpr{open, std::move(inner), false, 0});
        ExprP lazy = make_expr(UnaryExpr{"; }", std::move(head), true, 0});
        e = call_of("hot::safe_access", args_of(std::move(recv), std::move(lazy)), pos);
        return true;
    }

    // The statement "s" is an if or a while with a binding in the condition. An if becomes
    // "{ auto g = ...; if (g && rest) ... }". A while becomes
    // "for (;;) { auto g = ...; if (!(g && rest)) break; body }". Returns true when it did so.
    // The new statement is lowered here.
    bool bind_stmt(StmtP& s, ExprP& cond)
    {
        std::optional<Binding> bd = take_binding(cond);
        if (!bd)
            return false;
        auto decl =
            make_stmt(VarDeclStmt{false, false, bd->name, std::string(), std::move(bd->init), bd->pos, bd->name_pos});
        BlockStmt blk;
        blk.stmts.push_back(std::move(decl));
        if (auto* i = std::get_if<IfStmt>(&s->v))
        {
            blk.pos = i->pos;
            blk.stmts.push_back(make_stmt(std::move(*i)));
            s->v = std::move(blk);
        }
        else
        {
            WhileStmt& w = std::get<WhileStmt>(s->v);
            blk.pos = w.pos;
            auto unary = make_expr(UnaryExpr{"!", make_expr(ParenExpr{std::move(w.cond), 0}), false, 0});
            blk.stmts.push_back(make_stmt(IfStmt{std::move(unary), make_stmt(BreakStmt{w.pos}), nullptr, w.pos, 0}));
            blk.stmts.push_back(std::move(w.body));
            ForStmt f;
            f.pos = w.pos;
            f.body = make_stmt(std::move(blk));
            s->v = std::move(f);
        }
        stmt(s);
        return true;
    }

    void stmts(std::vector<StmtP>& v)
    {
        for (StmtP& s : v)
            stmt(s);
    }

    // A binding name goes out of scope at the end of its block.
    // The names that a block, a loop, a catch clause or a lambda declares end with it. A local
    // can hide a "[weak]" name only inside the scope. After the scope the old meaning is back.
    struct ScopeSave
    {
        RCtx& cx;
        std::set<std::string> locals, weak_locals, value_locals;
        std::map<std::string, std::string> local_type;
        explicit ScopeSave(RCtx& c)
            : cx(c)
            , locals(c.locals)
            , weak_locals(c.weak_locals)
            , value_locals(c.value_locals)
            , local_type(c.local_type)
        {
        }
        ~ScopeSave()
        {
            cx.locals = std::move(locals);
            cx.weak_locals = std::move(weak_locals);
            cx.value_locals = std::move(value_locals);
            cx.local_type = std::move(local_type);
        }
    };

    void block(BlockStmt& b)
    {
        const ScopeSave scope(cx);
        const size_t mark = bindings.size();
        stmts(b.stmts);
        bindings.resize(mark);
    }

    // The names of the structured bindings in scope ("var (a, b) = e" and "foreach (var (k, v)
    // in c)"). C++17 cannot capture a structured binding in a lambda (C++20 can). So a lambda
    // that uses a binding gets an init-capture "a = a" (or "&a = a"). An init-capture is C++14.
    std::vector<std::string> bindings;

    // The init-captures for the names in "scope" that the C++ text uses, for example ", a = a".
    // "by_ref" gives ", &a = a". A name in "skip" (a lambda parameter) gets no capture. A name
    // after ".", "->" or "::" and a name in a literal are not a use. An extra capture of an
    // unused name is not an error: it only makes a copy.
    static std::string binding_captures(const std::vector<std::string>& scope, const std::string& text, bool by_ref,
        const std::set<std::string>& skip = {})
    {
        if (scope.empty())
            return {};
        std::set<std::string> used;
        auto ident = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; };
        for (size_t i = 0; i < text.size();)
        {
            const char c = text[i];
            if (c == '"' || c == '\'')
            {
                for (++i; i < text.size() && text[i] != c; ++i)
                    if (text[i] == '\\')
                        ++i;
                ++i;
            }
            else if (std::isdigit(static_cast<unsigned char>(c)))
            {
                while (i < text.size() && (ident(text[i]) || text[i] == '.'))
                    ++i;
            }
            else if (ident(c))
            {
                const size_t start = i;
                while (i < text.size() && ident(text[i]))
                    ++i;
                size_t k = start;
                while (k > 0 && (text[k - 1] == ' ' || text[k - 1] == '\t' || text[k - 1] == '\n'))
                    --k;
                const bool member = k > 0
                    && (text[k - 1] == '.' || (k > 1 && text[k - 1] == '>' && text[k - 2] == '-')
                        || (k > 1 && text[k - 1] == ':' && text[k - 2] == ':'));
                if (!member)
                    used.insert(text.substr(start, i - start));
            }
            else
            {
                ++i;
            }
        }
        std::string r;
        std::set<std::string> done;
        for (const std::string& n : scope)
            if (used.count(n) && !skip.count(n) && done.insert(n).second)
                r += std::string(", ") + (by_ref ? "&" : "") + n + " = " + n;
        return r;
    }
    static std::string text_of(const BlockStmt& b)
    {
        std::string t;
        for (const StmtP& s : b.stmts)
            if (s)
                t += print(*s) + "\n";
        return t;
    }

    // The body of a loop. A "break" or "continue" in it targets the loop.
    void loop_body(StmtP& s)
    {
        ++fin.loops;
        stmt(s);
        --fin.loops;
    }

    void stmt(StmtP& s)
    {
        if (!s)
            return;
        Stmt& x = *s;
        if (auto* b = std::get_if<BlockStmt>(&x.v))
        {
            block(*b);
        }
        else if (auto* es = std::get_if<ExprStmt>(&x.v))
        {
            if (safe_stmt(s, *es))
                return;
            discard_next = true;
            expr(es->expr);
        }
        else if (auto* d = std::get_if<VarDeclStmt>(&x.v))
        {
            if (!d->names.empty())
            { // a structured binding: the names are plain locals
                for (const std::string& n : d->names)
                    declare(n, std::string());
                expr(d->init);
                bindings.insert(bindings.end(), d->names.begin(), d->names.end());
                return;
            }
            std::string t = d->type;
            if (t.empty() && d->init)
                t = type_of(*d->init); // "var x = new C()"
            // "C x" and "var x = valueExpr" hold a hot::Value. "var x = valueExpr" copies it.
            bool value_local = false;
            if (d->type.empty() && d->init && !d->is_ref && std::holds_alternative<NullExpr>(d->init->v))
            {
                error(pos_of(*d->init), kNullTypeError);
                d->init = nullptr;
                declare(d->name, std::string());
                return;
            }
            if (d->value)
            {
                if (!d->init)
                { // "C x;" runs "new C()"
                    NewExpr n;
                    n.type = d->type;
                    n.pos = d->pos;
                    d->init = make_expr(std::move(n));
                }
                value_local = true;
                if (cx.types && cx.types->value_storage(object_type(d->type)).empty())
                    error(d->pos,
                        "a script class value needs a std::shared_ptr handle, and '" + script_type_text(d->type)
                            + "' has not");
                else
                    value_wrap(d->init, d->type);
            }
            else if (d->type.empty() && d->init && !d->is_ref && is_value_ref(*d->init))
            {
                value_local = true;
            }
            else if (d->type.empty() && d->init && d->is_ref && is_value_ref(*d->init))
            {
                value_local = true; // "ref r = valueExpr" names the same box
            }
            else
            {
                value_not_to_handle(d->init, d->type);
            }
            if (t.empty())
                t = d->native_type; // a native call or member (S20b)
            // A collection literal needs the declared type: it is the C++ type of the braces.
            if (d->init && !d->type.empty())
                nested_collection(d->init);
            // "var me = this;" holds a handle of the current class.
            coerce(d->init, d->type.empty() && cx.cls ? cx.cls->name : d->type);
            // "ref r = w.f" or "ref r = w" of a weak handle: the lock is a temporary.
            if (d->is_ref && d->init)
                if (const std::string w = weak_root(*d->init); !w.empty())
                    weak_ref_error(d->pos, w);
            declare(d->name, t);
            if (value_local)
            {
                cx.value_locals.insert(d->name);
                if (d->value && cx.types)
                    if (const std::string v = cx.types->value_storage(object_type(d->type)); !v.empty())
                        d->type = v;
                expr(d->init);
                return;
            }
            if (d->weak)
            {
                // "[weak] T x": a weak_ptr of the shared handle of T. "= null" is the empty weak_ptr.
                cx.weak_locals.insert(d->name);
                reject_weak_new(d->init);
                if (d->init && std::holds_alternative<NullExpr>(d->init->v))
                    d->init = nullptr;
                if (cx.script_interfaces.count(object_type(d->type)))
                    error(d->weak_pos ? d->weak_pos : d->pos,
                        interface_attribute_error("weak", script_type_text(d->type)));
                else if (cx.types)
                {
                    const std::string w = cx.types->weak_storage(d->type);
                    if (w.empty())
                        error(d->weak_pos ? d->weak_pos : d->pos, weak_handle_error(script_type_text(d->type)));
                    else
                        d->type = w;
                }
            }
            else if (cx.types && !d->type.empty())
                d->type = cpp_type(d->type, TypeModel::StorageUse::Local, d->pos);
            expr(d->init);
        }
        else if (auto* i = std::get_if<IfStmt>(&x.v))
        {
            if (bind_stmt(s, i->cond))
                return;
            expr(i->cond);
            stmt(i->then);
            stmt(i->otherwise);
        }
        else if (auto* w = std::get_if<WhileStmt>(&x.v))
        {
            if (bind_stmt(s, w->cond))
                return;
            expr(w->cond);
            loop_body(w->body);
        }
        else if (auto* dw = std::get_if<DoWhileStmt>(&x.v))
        {
            loop_body(dw->body);
            expr(dw->cond);
        }
        else if (auto* f = std::get_if<ForStmt>(&x.v))
        {
            const ScopeSave scope(cx);
            stmts(f->init);
            expr(f->cond);
            for (ExprP& step : f->step)
            {
                discard_next = true;
                expr(step);
            }
            loop_body(f->body);
        }
        else if (auto* fo = std::get_if<ForOfStmt>(&x.v))
        {
            // "foreach (var x in w.items)" of a weak handle "w": the lock ends before the loop.
            if (fo->range)
                if (const std::string weak = weak_root(*fo->range); !weak.empty())
                    weak_ref_error(pos_of(*fo->range), weak);
            const ScopeSave scope(cx);
            // The type of the variable: the declared script class, else the element of a container.
            std::string elem;
            if (fo->range && fo->name.size())
                elem = element_type(type_of(*fo->range), true);
            if (!fo->type.empty())
                elem = class_named(object_type(fo->type)) ? fo->type : std::string();
            declare(fo->name, elem);
            if (fo->value)
            { // a bare script class variable holds a box: the range must hold boxes too
                cx.value_locals.insert(fo->name);
                const std::string held = fo->range ? container_element_type(type_of(*fo->range), true) : std::string();
                if (!held.empty() && held[0] == '$')
                    error(fo->pos, "foreach element type " + script_type_text(fo->type) + " does not match " + held
                            + "; write " + held + " " + fo->name);
            }
            for (const std::string& n : fo->names)
                declare(n, std::string());
            if (cx.types && !fo->type.empty())
                fo->type = cpp_type(fo->type, TypeModel::StorageUse::Local, fo->pos);
            check_ref_range(*fo);
            expr(fo->range);
            const size_t mark = bindings.size();
            bindings.insert(bindings.end(), fo->names.begin(), fo->names.end());
            loop_body(fo->body);
            bindings.resize(mark);
        }
        else if (auto* r = std::get_if<ReturnStmt>(&x.v))
        {
            if (fin.inside)
                error(r->pos, "'return' is not allowed in a 'finally' block");
            r->co = cx.async_body && !in_lambda;
            if (r->co)
                ++cx.co_keywords;
            if (cx.ret_value && !in_lambda)
                value_wrap(r->expr, cx.ret_type);
            else
            {
                coerce(r->expr, cx.ret_type);
                if (!in_lambda)
                    value_not_to_handle(r->expr, cx.ret_type);
            }
            expr(r->expr);
        }
        else if (auto* br = std::get_if<BreakStmt>(&x.v))
        {
            if (fin.inside && fin.loops == 0 && fin.switches == 0)
                error(br->pos, "'break' is not allowed in a 'finally' block");
        }
        else if (auto* co = std::get_if<ContinueStmt>(&x.v))
        {
            if (fin.inside && fin.loops == 0)
                error(co->pos, "'continue' is not allowed in a 'finally' block");
        }
        else if (auto* th = std::get_if<ThrowStmt>(&x.v))
        {
            // A script class is not an exception type. Only a struct with the base ScriptError is.
            if (th->expr)
            {
                const ClassDef* thrown = class_named(type_of(*th->expr));
                if (thrown && !thrown->is_value)
                    error(th->pos,
                        "cannot throw the script class '" + thrown->name + "': declare 'struct " + thrown->name
                            + " : ScriptError' for an exception");
            }
            expr(th->expr);
        }
        else if (auto* tr = std::get_if<TryStmt>(&x.v))
        {
            block(tr->block);
            for (CatchClause& c : tr->catches)
            {
                const ScopeSave scope(cx);
                if (!c.is_all)
                {
                    declare(c.name, std::string());
                    normalize_runtime_catch_type(c.type);
                }
                ++catch_depth;
                block(c.block);
                --catch_depth;
            }
            if (tr->finally_block)
            {
                const FinallyState outer = fin;
                fin = {true, 0, 0};
                block(*tr->finally_block);
                fin = outer;
                tr->finally_captures = binding_captures(bindings, text_of(*tr->finally_block), true);
                // In an async body the guard of a finally block can run when a reload cancelled
                // the frame: the owner destroys the frame later. Then the state block of the
                // generation is gone, so the body must not run. "hot_live" is a variable of the
                // file: the lambda sees it without a capture.
                if (cx.async_body && !in_lambda)
                    tr->finally_block->stmts.insert(tr->finally_block->stmts.begin(),
                        make_stmt(RawStmt{"if (HOT_UNLIKELY(!hot_live)) return;", 0}));
            }
        }
        else if (auto* sw = std::get_if<SwitchStmt>(&x.v))
        {
            const bool strings = classify_labels(*sw);
            expr(sw->subject);
            ++fin.switches;
            {
                const ScopeSave scope(cx); // the body of the switch is one scope
                for (SwitchCase& c : sw->cases)
                {
                    exprs(c.labels);
                    stmts(c.stmts);
                }
            }
            --fin.switches;
            if (strings)
                string_switch(s);
        }
    }

    // ---- switch on a string (DECISIONS item on string switch)
    // True when each label is a string literal. An error when only some labels are.
    bool classify_labels(const SwitchStmt& sw)
    {
        size_t strings = 0, others = 0;
        for (const SwitchCase& c : sw.cases)
            for (const ExprP& l : c.labels)
                (l && std::holds_alternative<StringExpr>(l->v) ? strings : others)++;
        if (strings == 0)
            return false;
        if (others == 0)
            return true;
        for (const SwitchCase& c : sw.cases)
            for (size_t i = 0; i < c.labels.size(); ++i)
                if (!c.labels[i] || !std::holds_alternative<StringExpr>(c.labels[i]->v))
                {
                    error(i < c.label_pos.size() ? c.label_pos[i] : c.pos,
                        "a switch cannot mix string labels and other labels");
                    return false;
                }
        return false;
    }

    // True when the statements end with a jump out of the case.
    static bool ends_with_jump(const std::vector<StmtP>& v)
    {
        if (v.empty() || !v.back())
            return false;
        const Stmt& l = *v.back();
        if (std::holds_alternative<BreakStmt>(l.v) || std::holds_alternative<ReturnStmt>(l.v)
            || std::holds_alternative<ThrowStmt>(l.v) || std::holds_alternative<ContinueStmt>(l.v))
            return true;
        if (auto* b = std::get_if<BlockStmt>(&l.v))
            return ends_with_jump(b->stmts);
        return false;
    }

    // Looks for a "break" that leaves the switch, or a "continue" that leaves the switch for an
    // outer loop. A loop owns the "break" and the "continue" in it. A nested switch owns the
    // "break" in it, but a "continue" in it goes to the outer loop.
    struct Jumps
    {
        bool brk = false, cont = false;
    };
    static void find_jumps(const std::vector<StmtP>& v, bool in_switch, Jumps& j)
    {
        for (const StmtP& s : v)
            find_jumps(s, in_switch, j);
    }
    static void find_jumps(const StmtP& s, bool in_switch, Jumps& j)
    {
        if (!s)
            return;
        const Stmt& x = *s;
        if (std::holds_alternative<BreakStmt>(x.v))
        {
            if (!in_switch)
                j.brk = true;
        }
        else if (std::holds_alternative<ContinueStmt>(x.v))
        {
            j.cont = true;
        }
        else if (auto* b = std::get_if<BlockStmt>(&x.v))
        {
            find_jumps(b->stmts, in_switch, j);
        }
        else if (auto* i = std::get_if<IfStmt>(&x.v))
        {
            find_jumps(i->then, in_switch, j);
            find_jumps(i->otherwise, in_switch, j);
        }
        else if (auto* t = std::get_if<TryStmt>(&x.v))
        {
            find_jumps(t->block.stmts, in_switch, j);
            for (const CatchClause& c : t->catches)
                find_jumps(c.block.stmts, in_switch, j);
            if (t->finally_block)
                find_jumps(t->finally_block->stmts, in_switch, j);
        }
        else if (auto* sw = std::get_if<SwitchStmt>(&x.v))
        {
            for (const SwitchCase& c : sw->cases)
                find_jumps(c.stmts, true, j);
        }
        // A loop owns its "break" and its "continue".
    }

    // A switch with string labels becomes a block. The block has a local with the value and an
    // if / else-if chain. C++ cannot switch on a string.
    //   { const auto& __sw_1 = s; if (__sw_1 == "a" || __sw_1 == "b") { ... } else { ... } }
    // A "break" at the end of a case is dropped. Another "break" that leaves the switch needs
    // a C++ construct, so the chain goes in "do { ... } while (false);".
    void string_switch(StmtP& s)
    {
        SwitchStmt& sw = std::get<SwitchStmt>(s->v);
        std::vector<SwitchCase>& cs = sw.cases;
        std::set<std::string> seen;
        Jumps jumps;
        for (size_t i = 0; i < cs.size(); ++i)
        {
            SwitchCase& c = cs[i];
            if (i + 1 < cs.size() && !ends_with_jump(c.stmts))
            {
                error(c.pos, "a string switch case must end with break, return, throw or continue");
                return;
            }
            for (size_t k = 0; k < c.labels.size(); ++k)
            {
                const std::string& t = std::get<StringExpr>(c.labels[k]->v).text;
                if (!c.is_default && !seen.insert(t).second)
                    error(k < c.label_pos.size() ? c.label_pos[k] : c.pos, "duplicate case label " + t);
            }
            if (!c.stmts.empty() && c.stmts.back() && std::holds_alternative<BreakStmt>(c.stmts.back()->v))
                c.stmts.pop_back();
            find_jumps(c.stmts, false, jumps);
        }
        if (jumps.brk && jumps.cont)
        {
            error(sw.pos, "a string switch cannot have both a nested 'break' and a 'continue'");
            return;
        }
        const std::string name = "__sw_" + std::to_string(++string_switches);
        auto body_of = [](SwitchCase& c, uint32_t pos)
        {
            BlockStmt b;
            b.pos = pos;
            b.stmts = std::move(c.stmts);
            return make_stmt(std::move(b));
        };
        StmtP chain;
        for (SwitchCase& c : cs)
            if (c.is_default)
                chain = body_of(c, c.default_pos != 0 ? c.default_pos : c.pos);
        for (size_t i = cs.size(); i-- > 0;)
        {
            SwitchCase& c = cs[i];
            if (c.is_default)
                continue;
            ExprP cond;
            for (ExprP& l : c.labels)
            {
                ExprP eq = make_expr(BinaryExpr{"==", raw(name, 0), std::move(l), 0});
                cond = cond ? make_expr(BinaryExpr{"||", std::move(cond), std::move(eq), 0}) : std::move(eq);
            }
            IfStmt n;
            n.cond = std::move(cond);
            n.then = body_of(c, 0);
            n.otherwise = std::move(chain);
            n.pos = c.pos;
            chain = make_stmt(std::move(n));
        }
        BlockStmt out;
        out.pos = sw.pos;
        out.end_pos = sw.end_pos;
        VarDeclStmt v;
        v.type = "const auto&";
        v.name = name;
        v.init = std::move(sw.subject);
        out.stmts.push_back(make_stmt(std::move(v)));
        if (chain && jumps.brk)
        {
            BlockStmt inner;
            inner.stmts.push_back(std::move(chain));
            DoWhileStmt dw;
            dw.body = make_stmt(std::move(inner));
            dw.cond = raw("false", 0);
            chain = make_stmt(std::move(dw));
        }
        if (chain)
            out.stmts.push_back(std::move(chain));
        s->v = std::move(out);
    }
};

} // namespace

std::string static_fn_symbol(const std::string& owner, const std::string& name)
{
    return owner.empty() ? "hot_fn_" + name : owner + "__s_" + name;
}

void lower_block(ast::BlockStmt& b, RCtx& cx, Diagnostics& diag)
{
    Lowerer{cx, diag}.block(b);
}

void lower_expr(ast::ExprP& e, RCtx& cx, Diagnostics& diag, const std::string& target_type)
{
    Lowerer lw{cx, diag};
    if (!target_type.empty())
        lw.coerce(e, target_type); // a class handle to an interface field or parameter
    if (cx.typed_init && e)
        lw.nested_collection(e);
    lw.expr(e);
}

void check_weak_new(const ast::ExprP& e, RCtx& cx, Diagnostics& diag)
{
    Lowerer{cx, diag}.reject_weak_new(e);
}

void lower_value_init(ast::ExprP& e, const std::string& type, RCtx& cx, Diagnostics& diag)
{
    Lowerer lw{cx, diag};
    if (!e)
    { // a "box T f;" field runs "new T()"
        ast::NewExpr n;
        n.type = type;
        e = make_expr(std::move(n));
    }
    lw.value_wrap(e, type);
    lw.expr(e);
}

} // namespace hotc
