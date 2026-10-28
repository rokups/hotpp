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
    for (const ast::Param& p : params) cx.locals.insert(p.name);
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
    };

    // The property that "this.P", a bare "P" or "obj.P" names. A local hides a bare "P".
    // The pass has no types: "obj.P" is a property when P is a property of the class.
    std::optional<PropRef> prop_ref(Expr& e) const
    {
        const std::string* n = nullptr;
        ExprP* obj = nullptr;
        if (const MemberExpr* m = this_member(e)) {
            n = &m->name;
        } else if (auto* m2 = std::get_if<MemberExpr>(&e.v)) {
            if (std::holds_alternative<SuperExpr>(m2->obj->v)) return std::nullopt;
            n = &m2->name;
            obj = &m2->obj;
        } else if ((n = simple_name(e)) && cx.locals.count(*n)) {
            n = nullptr;
        }
        if (!n || !cx.props.count(*n)) return std::nullopt;
        return PropRef{*n, obj};
    }

    // The checks of an accessor use. Gives the name of the accessor fn: "Cls__get_P".
    std::string accessor(const std::string& prop, bool is_get, uint32_t pos)
    {
        const char* word = is_get ? "get" : "set";
        if (!cx.allow_this)
            error(pos, "property '" + prop + "' is not allowed in a field initializer");
        else if (!(is_get ? cx.props[prop].has_get : cx.props[prop].has_set))
            error(pos, "property '" + prop + "' has no '" + word + "' accessor");
        return cx.cls->name + "__" + word + "_" + prop;
    }

    // The first argument of an accessor: "self", or "spp::shell_of<Cls_Shell>(obj, "Cls")"
    // for another object. The helper checks the class of the object at run time.
    ExprP receiver(PropRef& r, uint32_t pos)
    {
        if (!r.obj) return raw("self", 0);
        expr(*r.obj);
        CallExpr c;
        c.pos = pos;
        c.callee = raw("spp::shell_of<" + cx.cls->name + "_Shell>", pos);
        c.args.push_back(std::move(*r.obj));
        c.args.push_back(raw("\"" + cx.cls->name + "\"", 0));
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
        const std::string get = accessor(r.name, true, pos);
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
        const std::string get = op.empty() ? std::string() : accessor(r->name, true, pos);
        const std::string set = accessor(r->name, false, pos);
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
        c.callee = raw(cx.cls->name + "__" + fn, pos);
        c.args.insert(c.args.begin(), raw("self", 0));
    }

    void call(CallExpr& c)
    {
        if (const std::string* n = simple_name(*c.callee)) {
            if (cx.fns.count(*n) && !cx.locals.count(*n)) {
                script_call(c, *n, std::get<NameExpr>(c.callee->v).pos);
                exprs(c.args);
                return;
            }
        }
        if (const MemberExpr* m = this_member(*c.callee)) {
            if (cx.allow_this && !cx.field_type.count(m->name) && cx.fns.count(m->name)) {
                script_call(c, m->name, pos_of(*m->obj));
                exprs(c.args);
                return;
            }
        }
        expr(c.callee);
        exprs(c.args);
    }

    // new T(args). The type model gives the callee: a handle, a value or a raw type.
    void new_expr(ExprP& e, NewExpr& n)
    {
        const uint32_t pos = n.pos;
        if (cx.script_classes.count(n.type)) {
            error(pos, "new of a script class is not supported yet");
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
            if (cx.props.count(m.name)) {
                e = prop_read({m.name, nullptr}, obj_pos);  // "m" is gone now
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
            e = raw("self->" + cx.cls->base + "::" + m.name, obj_pos);
            return;
        }
        if (cx.props.count(m.name)) {  // "obj.P": a property of another object of the class
            e = prop_read({m.name, &m.obj}, obj_pos);  // "m" is gone now
            return;
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
        for (const Param& p : l.params) {
            // An empty type is "auto". Only a parameter without a name is an error.
            if (p.name.empty())
                error(l.pos, "a lambda parameter needs a name");
            cx.locals.insert(p.name);
        }
        if (cx.types)
            for (Param& p : l.params)
                if (!p.type.empty())
                    p.type = p.mode == PassMode::Value ? cx.types->param(p.type)
                                                       : cx.types->ref_param(p.type);
        // A "return" in a lambda leaves only the lambda. So it is correct in a finally block.
        const FinallyState outer = fin;
        fin = {};
        block(l.body);
        fin = outer;
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
    void name(ExprP& e, NameExpr& n)
    {
        targs(n);
        if (std::optional<PropRef> r = prop_ref(*e)) e = prop_read(std::move(*r), n.pos);
    }

    // A name with more than one part. If the first part is a local, the name is a member
    // chain: "a.b.c" becomes the node of "a.b.c" with MemberExpr. The lowering of it is
    // "spp::deref(spp::deref(a).b).c". Else it is a scope chain: the printer joins the
    // parts with "::". The change comes before the property rewrite, so "p.P" is "obj.P".
    void member_chain(ExprP& e)
    {
        auto* np = std::get_if<NameExpr>(&e->v);
        if (!np || np->parts.size() < 2 || !cx.locals.count(np->parts[0])) return;
        NameExpr& n = *np;
        targs(n);
        const uint32_t pos = n.pos;
        NameExpr head;
        head.parts = {n.parts[0]};
        head.pos = pos;
        ExprP chain = make_expr(std::move(head));
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
        }
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
            cx.locals.insert(d->name);
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
            cx.locals.insert(fo->name);
            if (cx.types && !fo->type.empty()) fo->type = cx.types->storage(fo->type);
            expr(fo->range);
            loop_body(fo->body);
        } else if (auto* r = std::get_if<ReturnStmt>(&x.v)) {
            if (fin.inside) error(r->pos, "'return' is not allowed in a 'finally' block");
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
                if (!c.is_all) cx.locals.insert(c.name);
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
