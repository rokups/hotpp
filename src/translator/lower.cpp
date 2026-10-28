// The lowering pass. See lower.h.
#include "lower.h"

#include "builtin_types.h"
#include "emit.h"

#include <optional>
#include <utility>

namespace sppc {

std::string rewrite_number(const std::string& n)
{
    if (n.size() > 1 && n[0] == '0' && (n[1] == 'x' || n[1] == 'X')) return n;
    bool fp = n.find_first_of(".eE") != std::string::npos;
    char last = n.back();
    if (fp && (last == 'd' || last == 'D')) return n.substr(0, n.size() - 1);
    if (fp && (last == 'f' || last == 'F')) return n;
    if (fp) return n + "f";
    if (last == 'L') return "int64_t{" + n.substr(0, n.size() - 1) + "}";
    return n;
}

RCtx make_class_ctx(const ast::ClassDef& cd)
{
    RCtx cx;
    cx.cls = &cd;
    for (const ast::Field& f : cd.fields) {
        cx.field_type[f.name] = f.type;
    }
    for (const ast::Method& m : cd.methods)
        if (!m.overrides_virtual) cx.fns.insert(m.name);
    for (const ast::Property& p : cd.props) cx.props[p.name] = {p.has_get, p.has_set};
    return cx;
}

void add_params(RCtx& cx, const std::vector<ast::Param>& params)
{
    for (const ast::Param& p : params) {
        cx.locals.insert(p.name);
        const std::string& t = p.script_type.empty() ? p.type : p.script_type;
        if (t.empty()) cx.local_type.erase(p.name);
        else cx.local_type[p.name] = t;
    }
}

namespace {

using namespace ast;

// A name with one part and no template arguments. Gives the part or null.
const std::string* simple_name(const Expr& e)
{
    const auto* n = std::get_if<NameExpr>(&e.v);
    if (!n || n->parts.size() != 1 || !n->template_args.empty()) return nullptr;
    return &n->parts[0];
}

// The member of "this". Gives the node or null.
const MemberExpr* this_member(const Expr& e)
{
    const auto* m = std::get_if<MemberExpr>(&e.v);
    if (!m || !std::holds_alternative<ThisExpr>(m->obj->v)) return nullptr;
    return m;
}

struct Lowerer {
    RCtx& cx;
    Diagnostics& diag;

    // The state of a finally block. The body of a finally block is a C++ lambda, so a jump
    // out of it is an error. A "return" in it is always an error. A "break" or "continue"
    // is an error when its target is outside the finally block.
    struct FinallyState {
        bool inside = false;  // true in the body of a finally block
        int loops = 0;        // the loops in the finally body around the current statement
        int switches = 0;     // the switches in the finally body around the current statement
    };
    FinallyState fin;

    void error(uint32_t pos, const std::string& msg)
    {
        if (cx.map) diag.add(pos, *cx.map, msg);
        else diag.add_line(0, msg);
    }

    ExprP raw(std::string text, uint32_t pos) { return make_expr(RawExpr{std::move(text), pos}); }

    // A property of the class. "obj" is null for "this.P" and a bare "P". Else it is the
    // object of "obj.P": another object of the class, before the lowering.
    struct PropRef {
        std::string name;
        ExprP* obj = nullptr;
        const ClassDef* cls = nullptr;  // the class that has the property
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
                if (p.name == n) {
                    if (owner) *owner = d;
                    return &p;
                }
        return nullptr;
    }

    // A field of the class or of a script parent.
    const Field* find_field(const ClassDef& c, const std::string& n) const
    {
        int depth = 0;
        for (const ClassDef* d = &c; d && depth < 64; d = parent_of(*d), ++depth)
            for (const Field& f : d->fields)
                if (f.name == n) return &f;
        return nullptr;
    }

    // A script function of the class or of a script parent: not an override of a native
    // virtual and not a hook. The nearest class wins. "owner" gets the class of the function.
    const Method* find_fn(const ClassDef& c, const std::string& n, const ClassDef** owner = nullptr) const
    {
        if (n == "onReload" || n.rfind("convert_", 0) == 0) return nullptr;
        int depth = 0;
        for (const ClassDef* d = &c; d && depth < 64; d = parent_of(*d), ++depth)
            for (const Method& m : d->methods)
                if (m.name == n && !m.overrides_virtual) {
                    if (owner) *owner = d;
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

    // The script type of an expression before its lowering. Empty: not known.
    std::string type_of(const Expr& e) const
    {
        if (std::holds_alternative<ThisExpr>(e.v)) return cx.allow_this && cx.cls ? cx.cls->name : std::string();
        if (const std::string* n = simple_name(e)) {
            if (!cx.locals.count(*n)) return {};
            auto it = cx.local_type.find(*n);
            return it == cx.local_type.end() ? std::string() : it->second;
        }
        if (auto* nw = std::get_if<NewExpr>(&e.v)) return class_named(nw->type) ? nw->type : std::string();
        if (auto* p = std::get_if<ParenExpr>(&e.v)) return type_of(*p->inner);
        if (auto* tt = std::get_if<TypeTestExpr>(&e.v)) {
            const ClassDef* d = tt->is_as ? class_named(tt->type) : nullptr;
            return d && !d->is_value ? tt->type : std::string();
        }
        if (auto* nm = std::get_if<NameExpr>(&e.v)) {  // a.b.c with a local head, before member_chain
            if (nm->parts.size() < 2 || !cx.locals.count(nm->parts[0])) return {};
            auto it = cx.local_type.find(nm->parts[0]);
            std::string t = it == cx.local_type.end() ? std::string() : it->second;
            for (size_t i = 1; i < nm->parts.size() && !t.empty(); ++i) t = member_type(t, nm->parts[i]);
            return t;
        }
        if (auto* m = std::get_if<MemberExpr>(&e.v)) {
            if (std::holds_alternative<SuperExpr>(m->obj->v)) return {};
            return member_type(type_of(*m->obj), m->name);
        }
        if (auto* c = std::get_if<CallExpr>(&e.v)) {
            if (const std::string* n = simple_name(*c->callee)) {
                if (cx.locals.count(*n) || !cx.cls) return {};
                const Method* fn = find_fn(*cx.cls, *n);
                return fn ? fn->script_ret : std::string();
            }
            if (auto* m = std::get_if<MemberExpr>(&c->callee->v)) {
                const ClassDef* d = class_named(type_of(*m->obj));
                const Method* fn = d ? find_fn(*d, m->name) : nullptr;
                return fn ? fn->script_ret : std::string();
            }
            if (auto* nm = std::get_if<NameExpr>(&c->callee->v)) {
                if (nm->parts.size() < 2) return {};
                std::string t = type_of(NameExprView(*nm, nm->parts.size() - 1));
                const ClassDef* d = class_named(t);
                const Method* fn = d ? find_fn(*d, nm->parts.back()) : nullptr;
                return fn ? fn->script_ret : std::string();
            }
        }
        return {};
    }

    // The type of the member "name" of an object of the type "t": a field or a property.
    std::string member_type(const std::string& t, const std::string& name) const
    {
        const ClassDef* d = class_named(t);
        if (!d) return {};
        if (const Field* f = find_field(*d, name)) return type_text(*f);
        if (const Property* p = find_prop(*d, name)) return p->script_type.empty() ? p->type : p->script_type;
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
        if (type.empty()) cx.local_type.erase(name);
        else cx.local_type[name] = type;
    }

    // "spp::shell_of<C_Shell>(obj, "C")" for the lowered text of the object.
    std::string shell_text(const ClassDef& d, const std::string& obj_text) const
    {
        return "spp::shell_of<" + d.name + "_Shell>(" + obj_text + ", \"" + d.name + "\")";
    }

    // Where a handle type is expected, a bare "this" becomes a handle (design 7.10.1).
    void coerce(ExprP& e, const std::string& type)
    {
        if (!e || type.empty() || !cx.types || !cx.allow_this) return;
        if (!std::holds_alternative<ThisExpr>(e->v)) return;
        const std::string h = cx.types->storage(type);
        if (h == type) return;  // not a handle type: "this" stays the pointer
        const uint32_t pos = std::get<ThisExpr>(e->v).pos;
        std::vector<ExprP> a;
        a.push_back(raw("self", 0));
        if (cx.handle_this) *cx.handle_this = true;
        e = call_of("spp::handle_from_this<" + h + ">", std::move(a), pos);
    }

    // The arguments of a call of the script function "fn" (null: no information).
    void coerce_args(CallExpr& c, const Method* fn, size_t first)
    {
        if (!fn) return;
        for (size_t i = first; i < c.args.size() && i - first < fn->params.size(); ++i) {
            const Param& p = fn->params[i - first];
            if (p.mode == PassMode::Value) coerce(c.args[i], p.script_type.empty() ? p.type : p.script_type);
        }
    }

    // The property that "this.P", a bare "P" or "obj.P" names. A local hides a bare "P".
    // The pass has no types: "obj.P" is a property when P is a property of the class.
    // "obj.P" uses the declared type of obj when it is known. Else P must be a property of
    // the current class.
    std::optional<PropRef> prop_ref(Expr& e) const
    {
        const std::string* n = nullptr;
        ExprP* obj = nullptr;
        const ClassDef* d = cx.cls;
        if (const MemberExpr* m = this_member(e)) {
            n = &m->name;
        } else if (auto* m2 = std::get_if<MemberExpr>(&e.v)) {
            if (std::holds_alternative<SuperExpr>(m2->obj->v)) return std::nullopt;
            n = &m2->name;
            obj = &m2->obj;
            const std::string t = type_of(*m2->obj);
            if (const ClassDef* td = class_named(t)) d = td;  // else: the old name rule
        } else if ((n = simple_name(e)) && cx.locals.count(*n)) {
            n = nullptr;
        }
        const ClassDef* owner = nullptr;
        if (!n || !d || !find_prop(*d, *n, &owner)) return std::nullopt;
        return PropRef{*n, obj, owner};
    }

    // The checks of an accessor use. Gives the name of the accessor fn: "Cls__get_P".
    std::string accessor(const ClassDef& d, const std::string& prop, bool is_get, uint32_t pos)
    {
        const char* word = is_get ? "get" : "set";
        const Property* p = find_prop(d, prop);
        if (!cx.allow_this)
            error(pos, "property '" + prop + "' is not allowed in a field initializer");
        else if (p && !(is_get ? p->has_get : p->has_set))
            error(pos, "property '" + prop + "' has no '" + word + "' accessor");
        return d.name + "__" + word + "_" + prop;
    }

    // The first argument of an accessor: "self", or "spp::shell_of<Cls_Shell>(obj, "Cls")"
    // for another object. The helper checks the class of the object at run time.
    ExprP receiver(PropRef& r, uint32_t pos)
    {
        if (!r.obj) return raw("self", 0);
        expr(*r.obj);
        CallExpr c;
        c.pos = pos;
        if (r.cls->is_value) {  // a [value] object: the address of the struct
            c.callee = raw("spp::value_ptr", pos);
            c.args.push_back(std::move(*r.obj));
            return make_expr(std::move(c));
        }
        c.callee = raw("spp::shell_of<" + r.cls->name + "_Shell>", pos);
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

    template <class... A>
    static std::vector<ExprP> args_of(A&&... a)
    {
        std::vector<ExprP> v;
        (v.push_back(std::move(a)), ...);
        return v;
    }

    // A read of a property: "Cls__get_P(receiver)".
    ExprP prop_read(PropRef r, uint32_t pos)
    {
        const std::string get = accessor(*r.cls, r.name, true, pos);
        return call_of(get, args_of(receiver(r, pos)), pos);
    }

    // "P = e", "P op= e", "++P", "--P", "P++" and "P--" on a property of "this" or of
    // another object. Gives false when the target is not a property.
    // In a statement ("discard" is true) the write is a call of the setter. Then a compound
    // write on "this" is "Cls__set_P(self, Cls__get_P(self) op (e))".
    // Else a helper of runtime.h makes the write. The helper gives the C# value and
    // evaluates the object once: spp::prop_set, spp::prop_update or spp::prop_post.
    bool prop_write(ExprP& e, bool discard)
    {
        static const std::map<std::string, std::string> op_names = {
            {"+", "op_add"}, {"-", "op_sub"}, {"*", "op_mul"}, {"/", "op_div"},  {"%", "op_mod"},
            {"&", "op_and"}, {"|", "op_or"},  {"^", "op_xor"}, {"<<", "op_shl"}, {">>", "op_shr"}};
        std::optional<PropRef> r;
        std::string op;  // empty for "=", else the operator without "="
        bool postfix = false, is_inc = false;
        ExprP value;
        uint32_t pos = 0;
        if (auto* b = std::get_if<BinaryExpr>(&e->v)) {
            const std::string& o = b->op;
            if (o != "=" && !(o.size() >= 2 && o.back() == '=' && op_names.count(o.substr(0, o.size() - 1))))
                return false;
            member_chain(b->lhs);
            if (!(r = prop_ref(*b->lhs))) return false;
            op = o.substr(0, o.size() - 1);
            pos = pos_of(*b->lhs);
            if (op.empty()) {
                const Property* pr = find_prop(*r->cls, r->name);
                if (pr) coerce(b->rhs, pr->script_type.empty() ? pr->type : pr->script_type);
            }
            expr(b->rhs);
            value = std::move(b->rhs);
        } else if (auto* u = std::get_if<UnaryExpr>(&e->v)) {
            if (u->op != "++" && u->op != "--") return false;
            member_chain(u->operand);
            if (!(r = prop_ref(*u->operand))) return false;
            op = u->op.substr(0, 1);
            postfix = u->postfix;
            is_inc = true;
            pos = pos_of(*u->operand);
            value = make_expr(NumberExpr{"1", pos});
        } else {
            return false;
        }
        const bool on_this = !r->obj;
        const std::string get = op.empty() ? std::string() : accessor(*r->cls, r->name, true, pos);
        const std::string set = accessor(*r->cls, r->name, false, pos);
        ExprP recv = receiver(*r, pos);
        if (discard && (op.empty() || on_this)) {
            if (!op.empty()) {
                BinaryExpr bin;
                bin.op = op;
                bin.lhs = call_of(get, args_of(raw("self", 0)), pos);
                // "++P" adds 1 without parentheses. "P op= e" puts e in parentheses.
                bin.rhs = is_inc ? std::move(value) : make_expr(ParenExpr{std::move(value), pos});
                bin.pos = pos;
                value = make_expr(std::move(bin));
            }
            e = call_of(set, args_of(std::move(recv), std::move(value)), pos);
        } else if (op.empty()) {
            e = call_of("spp::prop_set", args_of(raw(set, pos), std::move(recv), std::move(value)), pos);
        } else {
            const std::string helper = postfix && !discard ? "spp::prop_post" : "spp::prop_update";
            e = call_of(helper + "<spp::" + op_names.at(op) + ">",
                        args_of(raw(get, pos), raw(set, pos), std::move(recv), std::move(value)), pos);
        }
        return true;
    }

    void exprs(std::vector<ExprP>& v)
    {
        for (ExprP& x : v) expr(x);
    }

    // A script fn that "this.f" or "f" names. The call gets the "self" argument.
    void script_call(CallExpr& c, const std::string& fn, uint32_t pos)
    {
        const ClassDef* owner = cx.cls;
        const Method* m = find_fn(*cx.cls, fn, &owner);
        c.callee = raw(m ? fn_symbol(*owner, *m) : cx.cls->name + "__" + fn, pos);
        c.args.insert(c.args.begin(), raw("self", 0));
    }

    void call(CallExpr& c)
    {
        if (const std::string* n = simple_name(*c.callee)) {
            if (cx.fns.count(*n) && !cx.locals.count(*n)) {
                const Method* fn = find_fn(*cx.cls, *n);
                script_call(c, *n, std::get<NameExpr>(c.callee->v).pos);
                coerce_args(c, fn, 1);
                exprs(c.args);
                return;
            }
        }
        if (const MemberExpr* m = this_member(*c.callee)) {
            if (cx.allow_this && !cx.field_type.count(m->name) && cx.fns.count(m->name)) {
                const Method* fn = find_fn(*cx.cls, m->name);
                script_call(c, m->name, pos_of(*m->obj));
                coerce_args(c, fn, 1);
                exprs(c.args);
                return;
            }
        }
        // "super.M(args)" in a class with a script parent P (design 4.5): the implementation
        // that P sees, with a direct call. A native virtual M uses the helper "P__base_M".
        if (auto* sm = std::get_if<MemberExpr>(&c.callee->v);
            sm && std::holds_alternative<SuperExpr>(sm->obj->v) && cx.cls && cx.allow_this) {
            if (const ClassDef* par = parent_of(*cx.cls)) {
                const ClassDef* owner = nullptr;
                const Method* fn = find_fn(*par, sm->name, &owner);
                std::string callee;
                if (fn) callee = owner->name + "__" + sm->name;
                else if (cx.native_virtuals.count(sm->name)) callee = par->name + "__base_" + sm->name;
                if (!callee.empty()) {
                    c.callee = raw(callee, pos_of(*sm->obj));
                    c.args.insert(c.args.begin(), raw("self", 0));
                    if (fn) coerce_args(c, fn, 1);
                    for (size_t i = 1; i < c.args.size(); ++i) expr(c.args[i]);
                    return;
                }
            }
        }
        // "obj.M(args)" where obj has the type of a script class (not a [value] class) and M is
        // a script function of it: a direct call of the generation function.
        member_chain(c.callee);
        if (auto* mm = std::get_if<MemberExpr>(&c.callee->v)) {
            if (!std::holds_alternative<ThisExpr>(mm->obj->v) && !std::holds_alternative<SuperExpr>(mm->obj->v)) {
                const ClassDef* d = class_named(type_of(*mm->obj));
                const ClassDef* owner = d;
                const Method* fn =
                    d && !d->is_value && !find_prop(*d, mm->name) ? find_fn(*d, mm->name, &owner) : nullptr;
                if (fn) {
                    const uint32_t pos = pos_of(*mm->obj);
                    const std::string name = fn_symbol(*owner, *fn);
                    expr(mm->obj);
                    ExprP recv = raw(shell_text(*d, print(*mm->obj)), pos);
                    c.callee = raw(name, pos);
                    c.args.insert(c.args.begin(), std::move(recv));
                    coerce_args(c, fn, 1);
                    for (size_t i = 1; i < c.args.size(); ++i) expr(c.args[i]);
                    return;
                }
            }
        }
        expr(c.callee);
        exprs(c.args);
    }

    // new T(args). The type model gives the callee: a handle, a value or a raw type.
    void new_expr(ExprP& e, NewExpr& n)
    {
        const uint32_t pos = n.pos;
        const std::string type = n.type;  // "n" is gone after "e" gets a new node
        if (cx.script_classes.count(n.type)) {
            if (!cx.new_classes.count(n.type)) {
                error(pos, "class '" + n.type + "' must come before the class that builds it with 'new'");
                return;
            }
            exprs(n.args);
            CallExpr c;
            c.pos = pos;
            if (cx.types && cx.types->is_script_value(n.type)) {
                // A [value] class: "new Stats(args)" is the value "Stats(args)" (design 7.10.1).
                c.callee = raw(n.type, pos);
                c.args = std::move(n.args);
                e = make_expr(std::move(c));
                return;
            }
            c.callee = raw(n.type + "__new", pos);
            c.args = std::move(n.args);
            e = make_expr(std::move(c));
            // A script class is a handle (design 7.10.1): "H(Name__new(args))".
            if (cx.types) {
                const std::string h = cx.types->storage(type);
                if (h != type) {
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
        if (cx.types) callee = cx.types->make_callee(n.type);
        CallExpr c;
        c.pos = pos;
        if (!callee.wrap_new) {
            c.callee = raw(callee.text, pos);
            c.args = std::move(n.args);
        } else {
            CallExpr inner;
            inner.pos = pos;
            inner.callee = raw("new " + n.type, pos);
            inner.args = std::move(n.args);
            c.callee = raw(callee.text, pos);
            c.args.push_back(make_expr(std::move(inner)));
        }
        e = make_expr(std::move(c));
    }

    // Wraps an object in "spp::deref(...)". The helper gives a reference for a pointer and
    // for a reference. So the member access after it is the same for both.
    void deref(ExprP& obj)
    {
        const uint32_t pos = pos_of(*obj);
        CallExpr c;
        c.pos = pos;
        c.callee = raw("spp::deref", pos);
        c.args.push_back(std::move(obj));
        obj = make_expr(std::move(c));
    }

    void member(ExprP& e, MemberExpr& m)
    {
        const uint32_t obj_pos = pos_of(*m.obj);
        if (std::holds_alternative<ThisExpr>(m.obj->v)) {
            const ClassDef* prop_owner = nullptr;
            if (cx.cls && find_prop(*cx.cls, m.name, &prop_owner)) {
                e = prop_read({m.name, nullptr, prop_owner}, obj_pos);  // "m" is gone now
            } else if (!cx.allow_this) {
                error(obj_pos, "'this' is not allowed in a field initializer");
            } else if (cx.field_type.count(m.name)) {
                e = raw("st->" + m.name, obj_pos);
            } else if (cx.fns.count(m.name)) {
                error(obj_pos, "script function '" + m.name + "' must be called");
                e = raw(cx.cls->name + "__" + m.name, obj_pos);
            } else {
                e = raw("self->" + m.name, obj_pos);
            }
            return;
        }
        if (std::holds_alternative<SuperExpr>(m.obj->v)) {
            if (cx.cls->base.empty()) {
                error(obj_pos, "'super' needs a base class: '" + cx.cls->name + "' has none");
                return;
            }
            // With a script parent, the member is a member of the native base of the root.
            if (parent_of(*cx.cls)) {
                if (cx.native_base.empty()) {
                    error(obj_pos, "'super." + m.name + "' needs a native base: the classes of '" + cx.cls->name +
                                       "' have none");
                    return;
                }
                e = raw("self->" + cx.native_base + "::" + m.name, obj_pos);
                return;
            }
            e = raw("self->" + cx.cls->base + "::" + m.name, obj_pos);
            return;
        }
        // "obj.P", "obj.f": the type of obj decides. Without a type, P is a property of the
        // current class (the old rule).
        const std::string t = type_of(*m.obj);
        const ClassDef* td = class_named(t);
        const ClassDef* d = td ? td : cx.cls;  // a native type: the old name rule
        const ClassDef* prop_owner = nullptr;
        if (d && find_prop(*d, m.name, &prop_owner)) {
            e = prop_read({m.name, &m.obj, prop_owner}, obj_pos);  // "m" is gone now
            return;
        }
        if (td && !td->is_value) {
            if (find_field(*d, m.name)) {  // a field of a script object behind a handle
                expr(m.obj);
                e = raw("state_of(" + shell_text(*d, print(*m.obj)) + ")->" + m.name, obj_pos);
                return;
            }
            if (find_fn(*d, m.name)) {
                error(obj_pos, "script function '" + m.name + "' must be called");
                return;
            }
        }
        expr(m.obj);
        deref(m.obj);
    }

    void lambda(ExprP& e, LambdaExpr& l)
    {
        if (!cx.allow_this) {
            error(l.pos, "a lambda is not allowed in a field initializer");
            return;
        }
        if (cx.cls && cx.cls->is_value) {
            error(l.pos, "a lambda is not allowed in a [value] class: a value can move");
            return;
        }
        for (const Param& p : l.params) {
            // An empty type is "auto". Only a parameter without a name is an error.
            if (p.name.empty())
                error(l.pos, "a lambda parameter needs a name");
            declare(p.name, p.type);
        }
        if (cx.types)
            for (Param& p : l.params)
                if (!p.type.empty())
                    p.type = p.mode == PassMode::Value ? cx.types->param(p.type)
                                                       : cx.types->ref_param(p.type);
        // A "return" in a lambda leaves only the lambda. So it is correct in a finally block.
        const FinallyState outer = fin;
        const std::string outer_ret = std::exchange(cx.ret_type, std::string());
        fin = {};
        block(l.body);
        fin = outer;
        cx.ret_type = outer_ret;
        // The lambda captures by value. "self" is a pointer to the shell. The state pointer
        // is read again inside, so a lambda never keeps the pointer to a block.
        l.body.stmts.insert(l.body.stmts.begin(),
                            make_stmt(RawStmt{"[[maybe_unused]] auto* st = state_of(self);", 0}));
        const uint32_t pos = l.pos;
        CallExpr c;
        c.pos = pos;
        c.callee = raw("spp::make_closure", pos);
        c.args.push_back(raw("GEN", 0));
        c.args.push_back(raw("\"" + cx.where + " lambda\"", 0));
        c.args.push_back(std::move(e));
        e = make_expr(std::move(c));
    }

    // The template arguments that are expressions get the same lowering as other expressions.
    // The text changes only when the lowering changes an argument. So a type keeps its text.
    // The parser already wrote the type text with "::" for each scope.
    void targs(NameExpr& n)
    {
        if (!n.targ_parsed) return;
        auto joined = [&] {
            std::string r;
            for (const ExprP& a : n.targ_exprs) r += (r.empty() ? "" : ", ") + print(*a);
            return r;
        };
        const std::string before = joined();
        exprs(n.targ_exprs);
        const std::string after = joined();
        if (after != before) n.template_args = "<" + canonical_type(after) + ">";
    }

    // A bare name. A property of the class becomes a call of the getter.
    // True when the name is a module variable that no local hides.
    bool is_module_var(const std::string& n) const
    {
        return cx.module_vars.count(n) != 0 && cx.locals.count(n) == 0;
    }

    // The access to a module variable: "spp_mod()->name".
    ExprP module_ref(const std::string& n, uint32_t pos)
    {
        if (cx.in_module_init)
            error(pos, "the module variable '" + n + "' cannot be used in the initializer of a module variable");
        return raw("spp_mod()->" + n, pos);
    }

    void name(ExprP& e, NameExpr& n)
    {
        targs(n);
        if (n.parts.size() == 1 && is_module_var(n.parts[0])) {
            e = module_ref(n.parts[0], n.pos);
            return;
        }
        if (std::optional<PropRef> r = prop_ref(*e)) e = prop_read(std::move(*r), n.pos);
    }

    // A name with more than one part. If the first part is a local, the name is a member
    // chain: "a.b.c" becomes the node of "a.b.c" with MemberExpr. The lowering of it is
    // "spp::deref(spp::deref(a).b).c". Else it is a scope chain: the printer joins the
    // parts with "::". The change comes before the property rewrite, so "p.P" is "obj.P".
    void member_chain(ExprP& e)
    {
        auto* np = std::get_if<NameExpr>(&e->v);
        if (!np || np->parts.size() < 2) return;
        const bool module_head = is_module_var(np->parts[0]);
        if (!module_head && !cx.locals.count(np->parts[0])) return;
        NameExpr& n = *np;
        targs(n);
        const uint32_t pos = n.pos;
        NameExpr head;
        head.parts = {n.parts[0]};
        head.pos = pos;
        ExprP chain = module_head ? module_ref(n.parts[0], pos) : make_expr(std::move(head));
        for (size_t i = 1; i < n.parts.size(); ++i) {
            std::string member_name = n.parts[i];
            if (i + 1 == n.parts.size()) member_name += n.template_args;
            chain = make_expr(MemberExpr{std::move(chain), std::move(member_name), pos});
        }
        e = std::move(chain);  // "n" is gone now
    }

    // True for the next expr() call when the value of the expression is not used: the
    // expression of a statement and a step of a for loop.
    bool discard_next = false;

    void expr(ExprP& e)
    {
        const bool discard = std::exchange(discard_next, false);
        if (!e) return;
        member_chain(e);
        if (prop_write(e, discard)) return;
        Expr& x = *e;
        if (auto* n = std::get_if<ThisExpr>(&x.v)) {
            if (!cx.allow_this) error(n->pos, "'this' is not allowed in a field initializer");
            else e = raw("self", n->pos);
        } else if (auto* n2 = std::get_if<NullExpr>(&x.v)) {
            e = raw("nullptr", n2->pos);
        } else if (auto* nm = std::get_if<NameExpr>(&x.v)) {
            name(e, *nm);
        } else if (auto* num = std::get_if<NumberExpr>(&x.v)) {
            num->text = rewrite_number(num->text);
        } else if (auto* m = std::get_if<MemberExpr>(&x.v)) {
            member(e, *m);
        } else if (auto* c = std::get_if<CallExpr>(&x.v)) {
            call(*c);
        } else if (auto* ix = std::get_if<IndexExpr>(&x.v)) {
            expr(ix->obj);
            expr(ix->index);
        } else if (auto* u = std::get_if<UnaryExpr>(&x.v)) {
            expr(u->operand);
        } else if (auto* rf = std::get_if<RefExpr>(&x.v)) {
            expr(rf->operand);
        } else if (auto* b = std::get_if<BinaryExpr>(&x.v)) {
            if (b->op == "=" && b->lhs && b->rhs) coerce(b->rhs, type_of(*b->lhs));
            expr(b->lhs);
            expr(b->rhs);
        } else if (auto* t = std::get_if<TernaryExpr>(&x.v)) {
            expr(t->cond);
            expr(t->a);
            expr(t->b);
        } else if (auto* p = std::get_if<ParenExpr>(&x.v)) {
            expr(p->inner);
        } else if (auto* nw = std::get_if<NewExpr>(&x.v)) {
            new_expr(e, *nw);
        } else if (auto* l = std::get_if<LambdaExpr>(&x.v)) {
            lambda(e, *l);
        } else if (auto* tt = std::get_if<TypeTestExpr>(&x.v)) {
            type_test(e, *tt);
        } else if (auto* ip = std::get_if<InterpExpr>(&x.v)) {
            interp(e, *ip);
        }
    }

    // A string with "${...}" (design 7.3, DECISIONS item 10) becomes a call of std::format or
    // fmt::format. The literal parts become the format string: "{" and "}" are doubled and the
    // escape "\$" gives "$". A string with no argument stays a plain literal.
    void interp(ExprP& e, InterpExpr& n)
    {
        const uint32_t pos = n.pos;
        for (ExprP& a : n.args) expr(a);
        const bool has_args = !n.args.empty();
        std::string f = "\"";
        for (size_t i = 0; i < n.lits.size(); ++i) {
            const std::string& t = n.lits[i];
            for (size_t k = 0; k < t.size(); ++k) {
                if (t[k] == '\\' && k + 1 < t.size()) {
                    if (t[k + 1] != '$') f += t[k];
                    f += t[k + 1];
                    ++k;
                } else if (has_args && (t[k] == '{' || t[k] == '}')) {
                    f += t[k];
                    f += t[k];
                } else {
                    f += t[k];
                }
            }
            if (i < n.args.size()) f += n.specs[i].empty() ? "{}" : "{:" + n.specs[i] + "}";
        }
        f += "\"";
        if (!has_args) {
            e = raw(std::move(f), pos);
            return;
        }
        if (cx.uses_format) *cx.uses_format = true;
        CallExpr c;
        c.pos = pos;
        c.callee = raw(cx.format_lib == "fmt" ? "fmt::format" : "std::format", pos);
        c.args.push_back(raw(std::move(f), pos));
        for (ExprP& a : n.args) c.args.push_back(std::move(a));
        e = make_expr(std::move(c));
    }

    // "obj is T" and "obj as T" (design 7.7). A script class T uses the class records:
    // "spp::is_script(obj, "T")" and "spp::as_script<T_Shell>(obj, "T")". A native type T uses
    // RTTI: "spp::is_native<T>(obj)" and "spp::as_native<T>(obj)" (dynamic_cast).
    void type_test(ExprP& e, TypeTestExpr& t)
    {
        const uint32_t pos = t.pos;
        const std::string type = t.type;
        const ClassDef* d = class_named(type);
        if (d && d->is_value) {
            error(pos, std::string("'") + (t.is_as ? "as" : "is") + "' needs a class with a handle: '" + type +
                           "' is a [value] class");
            return;
        }
        expr(t.obj);
        CallExpr c;
        c.pos = pos;
        if (d) c.callee = raw(t.is_as ? "spp::as_script<" + type + "_Shell>" : std::string("spp::is_script"), pos);
        else c.callee = raw((t.is_as ? "spp::as_native<" : "spp::is_native<") + type + ">", pos);
        c.args.push_back(std::move(t.obj));
        if (d) c.args.push_back(raw("\"" + type + "\"", pos));
        e = make_expr(std::move(c));
    }

    void stmts(std::vector<StmtP>& v)
    {
        for (StmtP& s : v) stmt(s);
    }

    void block(BlockStmt& b) { stmts(b.stmts); }

    // The body of a loop. A "break" or "continue" in it targets the loop.
    void loop_body(StmtP& s)
    {
        ++fin.loops;
        stmt(s);
        --fin.loops;
    }

    void stmt(StmtP& s)
    {
        if (!s) return;
        Stmt& x = *s;
        if (auto* b = std::get_if<BlockStmt>(&x.v)) {
            block(*b);
        } else if (auto* es = std::get_if<ExprStmt>(&x.v)) {
            discard_next = true;
            expr(es->expr);
        } else if (auto* d = std::get_if<VarDeclStmt>(&x.v)) {
            std::string t = d->type;
            if (t.empty() && d->init) t = type_of(*d->init);  // "var x = new C()"
            coerce(d->init, d->type);
            declare(d->name, t);
            if (cx.types && !d->type.empty()) d->type = cx.types->storage(d->type);
            expr(d->init);
        } else if (auto* i = std::get_if<IfStmt>(&x.v)) {
            expr(i->cond);
            stmt(i->then);
            stmt(i->otherwise);
        } else if (auto* w = std::get_if<WhileStmt>(&x.v)) {
            expr(w->cond);
            loop_body(w->body);
        } else if (auto* dw = std::get_if<DoWhileStmt>(&x.v)) {
            loop_body(dw->body);
            expr(dw->cond);
        } else if (auto* f = std::get_if<ForStmt>(&x.v)) {
            stmts(f->init);
            expr(f->cond);
            for (ExprP& step : f->step) {
                discard_next = true;
                expr(step);
            }
            loop_body(f->body);
        } else if (auto* fo = std::get_if<ForOfStmt>(&x.v)) {
            declare(fo->name, std::string());
            if (cx.types && !fo->type.empty()) fo->type = cx.types->storage(fo->type);
            expr(fo->range);
            loop_body(fo->body);
        } else if (auto* r = std::get_if<ReturnStmt>(&x.v)) {
            if (fin.inside) error(r->pos, "'return' is not allowed in a 'finally' block");
            coerce(r->expr, cx.ret_type);
            expr(r->expr);
        } else if (auto* br = std::get_if<BreakStmt>(&x.v)) {
            if (fin.inside && fin.loops == 0 && fin.switches == 0)
                error(br->pos, "'break' is not allowed in a 'finally' block");
        } else if (auto* co = std::get_if<ContinueStmt>(&x.v)) {
            if (fin.inside && fin.loops == 0)
                error(co->pos, "'continue' is not allowed in a 'finally' block");
        } else if (auto* th = std::get_if<ThrowStmt>(&x.v)) {
            expr(th->expr);
        } else if (auto* tr = std::get_if<TryStmt>(&x.v)) {
            block(tr->block);
            for (CatchClause& c : tr->catches) {
                if (!c.is_all) declare(c.name, std::string());
                block(c.block);
            }
            if (tr->finally_block) {
                const FinallyState outer = fin;
                fin = {true, 0, 0};
                block(*tr->finally_block);
                fin = outer;
            }
        } else if (auto* sw = std::get_if<SwitchStmt>(&x.v)) {
            expr(sw->subject);
            ++fin.switches;
            for (SwitchCase& c : sw->cases) {
                exprs(c.labels);
                stmts(c.stmts);
            }
            --fin.switches;
        }
    }
};

}  // namespace

void lower_block(ast::BlockStmt& b, RCtx& cx, Diagnostics& diag) { Lowerer{cx, diag}.block(b); }

void lower_expr(ast::ExprP& e, RCtx& cx, Diagnostics& diag) { Lowerer{cx, diag}.expr(e); }

}  // namespace sppc
