// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The lowering pass. See lower.h.
#include "lower.h"

#include "builtin_types.h"
#include "emit.h"

#include <cctype>
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
// "const hot::X&". Another type text stays as it is.
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
    if (t == "NullReference" || t == "IndexOutOfRange" || t == "ScriptError")
        type = "const hot::" + t + "&";
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
        auto it = cx.classes.find(t);
        return it == cx.classes.end() ? nullptr : it->second;
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
        return (d && !d->is_value) || !container_element_type(t).empty() ? t : std::string();
    }

    // The script type of an expression before its lowering. Empty: not known.
    std::string type_of(const Expr& e) const
    {
        if (auto* ix = std::get_if<IndexExpr>(&e.v))
            return ix->obj ? element_type(type_of(*ix->obj)) : std::string();
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
        if (auto* b = std::get_if<BinaryExpr>(&e.v))
        {
            if (b->op != "??" || !b->lhs || !b->rhs)
                return {};
            const std::string t = common_type(type_of(*b->lhs), type_of(*b->rhs));
            const ClassDef* d = class_named(t);
            return d && !d->is_value ? t : std::string();
        }
        if (auto* tt = std::get_if<TypeTestExpr>(&e.v))
        {
            const ClassDef* d = tt->is_as ? class_named(tt->type) : nullptr;
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
            return member_type(type_of(*m->obj), m->name);
        }
        if (auto* c = std::get_if<CallExpr>(&e.v))
        {
            if (const std::string* n = simple_name(*c->callee))
            {
                if (cx.locals.count(*n) || !cx.cls)
                    return {};
                const Method* fn = find_fn(*cx.cls, *n);
                return fn ? fn->script_ret : std::string();
            }
            if (auto* m = std::get_if<MemberExpr>(&c->callee->v))
            {
                const std::string ot = type_of(*m->obj);
                if (m->name == "front" || m->name == "back" || m->name == "at")
                    if (const std::string el = element_type(ot); !el.empty())
                        return el;
                const ClassDef* d = class_named(ot);
                const Method* fn = d ? find_fn(*d, m->name) : nullptr;
                return fn ? fn->script_ret : std::string();
            }
            if (auto* nm = std::get_if<NameExpr>(&c->callee->v))
            {
                if (nm->parts.size() < 2)
                    return {};
                std::string t = type_of(NameExprView(*nm, nm->parts.size() - 1));
                const std::string& last = nm->parts.back();
                if (last == "front" || last == "back" || last == "at")
                    if (const std::string el = element_type(t); !el.empty())
                        return el;
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

    // Where a handle type is expected, a bare "this" becomes a handle (design 7.10.1).
    void coerce(ExprP& e, const std::string& type)
    {
        if (!e || type.empty() || !cx.types || !cx.allow_this)
            return;
        if (!std::holds_alternative<ThisExpr>(e->v))
            return;
        const std::string h = cx.types->storage(type);
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
            if (p.weak)
            { // "null" for a "[weak] T" parameter: the empty weak_ptr (a weak_ptr has no nullptr form)
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
                        + object_type(t) + "'. Copy it to a local first: '" + object_type(t) + " me = this;'");
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
        if (!cx.allow_this)
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
        { // a [value] object: the address of the struct
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
    // A script class is a handle, unless it is a [value] class. A native class is a handle
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

    void call(CallExpr& c)
    {
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
        // "obj.M(args)" where obj has the type of a script class (not a [value] class) and M is
        // a script function of it: a direct call of the generation function.
        member_chain(c.callee);
        if (auto* mm = std::get_if<MemberExpr>(&c.callee->v))
        {
            if (!std::holds_alternative<ThisExpr>(mm->obj->v) && !std::holds_alternative<BaseExpr>(mm->obj->v))
            {
                const ClassDef* d = class_named(type_of(*mm->obj));
                const ClassDef* owner = d;
                const Method* fn =
                    d && !d->is_value && !find_prop(*d, mm->name) ? find_fn(*d, mm->name, &owner) : nullptr;
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
        const std::string type = n.type; // "n" is gone after "e" gets a new node
        if (cx.script_classes.count(n.type))
        {
            if (!cx.new_classes.count(n.type))
            {
                error(pos, "class '" + n.type + "' must come before the class that builds it with 'new'");
                return;
            }
            check_ctor_access(n);
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
                // A [value] class: "new Stats(args)" is the value "Stats(args)" (design 7.10.1).
                c.callee = raw(n.type, pos);
                e = make_expr(std::move(c));
                return;
            }
            // A script class is a handle (design 7.10.1): "H(Name__new(args))". A std::shared_ptr
            // handle uses "H(Name__new_shared(args))": one block for the shell and the control block.
            // make_shared_shell gives a std::shared_ptr. So the shared pointer of another standard
            // namespace (the module attribute "std") uses "H(Name__new(args))".
            const std::string h = cx.types ? cx.types->storage(type) : type;
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
        if (m.safe)
            error(pos_of(*m.obj), "'?.' is allowed only at the start of an expression statement");
        const uint32_t obj_pos = pos_of(*m.obj);
        if (cx.in_default && (std::holds_alternative<ThisExpr>(m.obj->v) || std::holds_alternative<BaseExpr>(m.obj->v)))
        {
            error(obj_pos, kDefaultBan);
            return;
        }
        if (std::holds_alternative<ThisExpr>(m.obj->v))
        {
            const ClassDef* prop_owner = nullptr;
            if (cx.cls && find_prop(*cx.cls, m.name, &prop_owner))
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
        }
        // A "[weak]" field of a script struct: "hot::lock(hot::deref(s).w)".
        const bool weak_value = td && td->is_value && weak_field_of(*td, m.name) && !target;
        expr(m.obj);
        deref(m.obj);
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
        if (!cx.allow_this)
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
        }
        if (cx.types)
            for (Param& p : l.params)
                if (!p.type.empty())
                    p.type = cx.types->param(p.type);
        // A "return" in a lambda leaves only the lambda. So it is correct in a finally block.
        const FinallyState outer = fin;
        const std::string outer_ret = std::exchange(cx.ret_type, std::string());
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
        n.template_args = cx.types->targ_storage(last + n.template_args).substr(last.size());
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
            error(n.pos, "'this' is not allowed in a field initializer");
            return;
        }
        if (bare)
        {
            // A bare field of a [value] class: its functions are free functions, "__st" is the object.
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
        Expr& x = *e;
        if (auto* n = std::get_if<ThisExpr>(&x.v))
        {
            if (cx.in_default)
                error(n->pos, kDefaultBan);
            else if (!cx.allow_this)
                error(n->pos, "'this' is not allowed in a field initializer");
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
            expr(ix->obj);
            expr(ix->index);
        }
        else if (auto* u = std::get_if<UnaryExpr>(&x.v))
        {
            if (u->op == "&" && !u->postfix && u->operand)
                if (const std::string w = weak_root(*u->operand); !w.empty())
                    weak_ref_error(u->pos ? u->pos : pos_of(*u->operand), w);
            if (u->op == "await" && !u->postfix)
            {
                check_await(u->pos);
                if (!in_lambda)
                    ++cx.co_keywords;
            }
            expr(u->operand);
        }
        else if (auto* b = std::get_if<BinaryExpr>(&x.v))
        {
            if (b->op == "=" && b->lhs && b->rhs)
                coerce(b->rhs, type_of(*b->lhs));
            // "w = e" writes the weak handle "w": the target is not locked. "w = null" resets it.
            if (b->op == "=" && b->lhs)
                member_chain(b->lhs);
            const bool weak_lhs = b->op == "=" && b->lhs && is_weak_handle(*b->lhs);
            const bool reset = weak_lhs && b->rhs && std::holds_alternative<NullExpr>(b->rhs->v);
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
        expr(t.obj);
        CallExpr c;
        c.pos = pos;
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

    // A new node for a name without template arguments.
    static ExprP copy_name(const NameExpr& n)
    {
        NameExpr c;
        c.parts = n.parts;
        c.pos = n.pos;
        c.last_pos = n.last_pos;
        return make_expr(std::move(c));
    }

    // "foreach (var& x in enumerate(xs))": the range gives temporary values, so a reference
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
        error(callee->pos, "'var&' cannot bind to the temporary value of '" + helper + "'; use '" + use + "'");
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
        std::set<std::string> locals, weak_locals;
        std::map<std::string, std::string> local_type;
        explicit ScopeSave(RCtx& c)
            : cx(c)
            , locals(c.locals)
            , weak_locals(c.weak_locals)
            , local_type(c.local_type)
        {
        }
        ~ScopeSave()
        {
            cx.locals = std::move(locals);
            cx.weak_locals = std::move(weak_locals);
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
            if (t.empty())
                t = d->native_type; // a native call or member (S20b)
            // A collection literal needs the declared type: it is the C++ type of the braces.
            if (d->init && !d->type.empty())
                nested_collection(d->init);
            // "var me = this;" holds a handle of the current class.
            coerce(d->init, d->type.empty() && cx.cls ? cx.cls->name : d->type);
            // "var& r = w.f" or "var& r = w" of a weak handle: the lock is a temporary.
            if (d->is_ref && d->init)
                if (const std::string w = weak_root(*d->init); !w.empty())
                    weak_ref_error(d->pos, w);
            declare(d->name, t);
            if (d->weak)
            {
                // "[weak] T x": a weak_ptr of the shared handle of T. "= null" is the empty weak_ptr.
                cx.weak_locals.insert(d->name);
                if (d->init && std::holds_alternative<NullExpr>(d->init->v))
                    d->init = nullptr;
                if (cx.types)
                {
                    const std::string w = cx.types->weak_storage(d->type);
                    if (w.empty())
                        error(d->weak_pos ? d->weak_pos : d->pos, weak_handle_error(script_type_text(d->type)));
                    else
                        d->type = w;
                }
            }
            else if (cx.types && !d->type.empty())
                d->type = cx.types->storage(d->type);
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
            for (const std::string& n : fo->names)
                declare(n, std::string());
            if (cx.types && !fo->type.empty())
                fo->type = cx.types->storage(fo->type);
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
            coerce(r->expr, cx.ret_type);
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

void lower_block(ast::BlockStmt& b, RCtx& cx, Diagnostics& diag)
{
    Lowerer{cx, diag}.block(b);
}

void lower_expr(ast::ExprP& e, RCtx& cx, Diagnostics& diag)
{
    Lowerer lw{cx, diag};
    if (cx.typed_init && e)
        lw.nested_collection(e);
    lw.expr(e);
}

} // namespace hotc
