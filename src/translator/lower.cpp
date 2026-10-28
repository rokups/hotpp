// The lowering pass. See lower.h.
#include "lower.h"

#include "emit.h"

namespace spc {

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

    void error(uint32_t pos, const std::string& msg)
    {
        if (cx.map) diag.add(pos, *cx.map, msg);
        else diag.add_line(0, msg);
    }

    ExprP raw(std::string text, uint32_t pos) { return make_expr(RawExpr{std::move(text), pos}); }

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

    // Wraps an object in "script::deref(...)". The helper gives a reference for a pointer and
    // for a reference. So the member access after it is the same for both.
    void deref(ExprP& obj)
    {
        const uint32_t pos = pos_of(*obj);
        CallExpr c;
        c.pos = pos;
        c.callee = raw("script::deref", pos);
        c.args.push_back(std::move(obj));
        obj = make_expr(std::move(c));
    }

    void member(ExprP& e, MemberExpr& m)
    {
        const uint32_t obj_pos = pos_of(*m.obj);
        if (std::holds_alternative<ThisExpr>(m.obj->v)) {
            if (!cx.allow_this) {
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
            if (p.name.empty() || p.type.empty())
                error(l.pos, "a lambda parameter needs the form 'name: type'");
            cx.locals.insert(p.name);
        }
        block(l.body);
        // The lambda captures by value. "self" is a pointer to the shell. The state pointer
        // is read again inside, so a lambda never keeps the pointer to a block.
        l.body.stmts.insert(l.body.stmts.begin(),
                            make_stmt(RawStmt{"[[maybe_unused]] auto* st = state_of(self);", 0}));
        const uint32_t pos = l.pos;
        CallExpr c;
        c.pos = pos;
        c.callee = raw("script::make_closure", pos);
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
        if (after != before) n.template_args = "<" + after + ">";
    }

    // A name with more than one part. If the first part is a local, the name is a member
    // chain: "a.b.c" is "script::deref(script::deref(a).b).c". Else it is a scope chain:
    // the printer joins the parts with "::".
    void name(ExprP& e, NameExpr& n)
    {
        targs(n);
        if (n.parts.size() < 2 || !cx.locals.count(n.parts[0])) return;
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
        expr(e);
    }

    void expr(ExprP& e)
    {
        if (!e) return;
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
        } else if (auto* b = std::get_if<BinaryExpr>(&x.v)) {
            expr(b->lhs);
            expr(b->rhs);
        } else if (auto* t = std::get_if<TernaryExpr>(&x.v)) {
            expr(t->cond);
            expr(t->a);
            expr(t->b);
        } else if (auto* p = std::get_if<ParenExpr>(&x.v)) {
            expr(p->inner);
        } else if (auto* bl = std::get_if<BracedListExpr>(&x.v)) {
            exprs(bl->elems);
        } else if (auto* tb = std::get_if<TypedBracedExpr>(&x.v)) {
            exprs(tb->elems);
        } else if (auto* l = std::get_if<LambdaExpr>(&x.v)) {
            lambda(e, *l);
        }
    }

    void stmts(std::vector<StmtP>& v)
    {
        for (StmtP& s : v) stmt(s);
    }

    void block(BlockStmt& b) { stmts(b.stmts); }

    void stmt(StmtP& s)
    {
        if (!s) return;
        Stmt& x = *s;
        if (auto* b = std::get_if<BlockStmt>(&x.v)) {
            block(*b);
        } else if (auto* es = std::get_if<ExprStmt>(&x.v)) {
            expr(es->expr);
        } else if (auto* d = std::get_if<VarDeclStmt>(&x.v)) {
            cx.locals.insert(d->name);
            expr(d->init);
        } else if (auto* i = std::get_if<IfStmt>(&x.v)) {
            expr(i->cond);
            stmt(i->then);
            stmt(i->otherwise);
        } else if (auto* w = std::get_if<WhileStmt>(&x.v)) {
            expr(w->cond);
            stmt(w->body);
        } else if (auto* dw = std::get_if<DoWhileStmt>(&x.v)) {
            stmt(dw->body);
            expr(dw->cond);
        } else if (auto* f = std::get_if<ForStmt>(&x.v)) {
            stmts(f->init);
            expr(f->cond);
            exprs(f->step);
            stmt(f->body);
        } else if (auto* fo = std::get_if<ForOfStmt>(&x.v)) {
            cx.locals.insert(fo->name);
            expr(fo->range);
            stmt(fo->body);
        } else if (auto* r = std::get_if<ReturnStmt>(&x.v)) {
            expr(r->expr);
        } else if (auto* th = std::get_if<ThrowStmt>(&x.v)) {
            expr(th->expr);
        } else if (auto* tr = std::get_if<TryStmt>(&x.v)) {
            block(tr->block);
            for (CatchClause& c : tr->catches) {
                if (!c.is_all) cx.locals.insert(c.name);
                block(c.block);
            }
        } else if (auto* sw = std::get_if<SwitchStmt>(&x.v)) {
            expr(sw->subject);
            for (SwitchCase& c : sw->cases) {
                exprs(c.labels);
                stmts(c.stmts);
            }
        }
    }
};

}  // namespace

void lower_block(ast::BlockStmt& b, RCtx& cx, Diagnostics& diag) { Lowerer{cx, diag}.block(b); }

void lower_expr(ast::ExprP& e, RCtx& cx, Diagnostics& diag) { Lowerer{cx, diag}.expr(e); }

}  // namespace spc
