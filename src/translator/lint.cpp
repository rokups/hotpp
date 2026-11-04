// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The lint pass (lint.h). One walk over each body keeps a stack of scopes. A name is a use when a
// NameExpr starts with it. The only exception is "x = e;" as a statement: it writes "x" and does
// not read it.
#include "lint.h"

#include <algorithm>
#include <map>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace hotc
{

using namespace ast;

namespace
{

enum class Kind
{
    Param, // a parameter of the function or of a lambda
    Local, // a local with "var" or a type: the unused check applies
    Other, // a foreach variable, a catch variable, a "var (a, b)" name: no unused check
    Binding, // an "is T name" binding: no unused check
};

struct Var
{
    Kind kind = Kind::Local;
    uint32_t pos = 0;
    bool is_ref = false; // a write to a "ref" local writes the target: it counts as a use
    bool used = false;
};

uint32_t stmt_pos(const Stmt& s)
{
    return std::visit([](const auto& n) -> uint32_t { return n.pos; }, s.v);
}

// One callable from many lambdas, for std::visit.
template <class... Fs> struct Overloaded : Fs...
{
    using Fs::operator()...;
};
template <class... Fs> Overloaded(Fs...) -> Overloaded<Fs...>;

class Linter
{
public:
    Linter(const LineMap& map, Diagnostics& diag) : map_(map), diag_(diag) {}

    void body(const std::vector<Param>& params, const BlockStmt& b, const char* extra = nullptr)
    {
        scopes_.clear();
        push();
        for (const Param& p : params)
            declare(p.name, Kind::Param, b.pos, false);
        if (extra)
            declare(extra, Kind::Param, b.pos, false);
        block(b);
        pop();
        flush();
    }

    void init(const ExprP& e)
    {
        scopes_.clear();
        push();
        expr(e);
        pop();
        flush();
    }

private:
    using Scope = std::map<std::string, Var>;

    const LineMap& map_;
    Diagnostics& diag_;
    std::vector<Scope> scopes_;
    // The unused locals of the body. A body with C++ text that the pass cannot read (a RawExpr or a
    // RawStmt) can use any name, so such a body gives no "unused" warning.
    std::vector<std::pair<uint32_t, std::string>> unused_;
    bool opaque_ = false;

    void flush()
    {
        if (!opaque_)
            for (const auto& [pos, name] : unused_)
                diag_.warn(pos, map_, "local variable '" + name + "' is never used. Remove it, or start its name with '_'");
        unused_.clear();
        opaque_ = false;
    }

    void push() { scopes_.emplace_back(); }

    void pop()
    {
        for (const auto& [name, v] : scopes_.back())
            if (v.kind == Kind::Local && !v.used && name[0] != '_')
                unused_.emplace_back(v.pos, name);
        scopes_.pop_back();
    }

    static bool exempt(const std::string& name) { return name.empty() || name[0] == '_'; }

    // Adds a name to the top scope. A name that an outer scope has gives a shadow warning.
    void declare(const std::string& name, Kind kind, uint32_t pos, bool is_ref, bool check_shadow = true)
    {
        if (name.empty() || scopes_.back().count(name))
            return; // the checker reports a second declaration in the same scope
        if (check_shadow && name != "_" && pos != 0)
        {
            for (size_t i = scopes_.size() - 1; i-- > 0;)
            {
                auto it = scopes_[i].find(name);
                if (it == scopes_[i].end())
                    continue;
                if (it->second.kind == Kind::Param)
                    diag_.warn(pos, map_, "'" + name + "' hides a parameter with the same name");
                else
                    diag_.warn(pos, map_,
                        "'" + name + "' hides the " + (it->second.kind == Kind::Binding ? "pattern binding" : "local variable")
                            + " '" + name + "' that is declared at line " + std::to_string(map_.line(it->second.pos)));
                break;
            }
        }
        Var v;
        v.kind = kind;
        v.pos = pos;
        v.is_ref = is_ref;
        scopes_.back()[name] = v;
    }

    Var* find(const std::string& name)
    {
        for (size_t i = scopes_.size(); i-- > 0;)
        {
            auto it = scopes_[i].find(name);
            if (it != scopes_[i].end())
                return &it->second;
        }
        return nullptr;
    }

    void use(const std::string& name)
    {
        if (Var* v = find(name))
            v->used = true;
    }

    // ---------------------------------------------------------------- expressions
    void expr(const ExprP& e)
    {
        if (e)
            expr(*e);
    }

    void exprs(const std::vector<ExprP>& list)
    {
        for (const ExprP& e : list)
            expr(e);
    }

    // The visitor has one overload for each kind of node and no catch-all. A new kind of node does not
    // compile until this pass handles it. A leaf kind has an empty overload.
    void expr(const Expr& e)
    {
        std::visit(
            Overloaded{
                [&](const NameExpr& n)
                {
                    if (!n.parts.empty())
                        use(n.parts[0]);
                    exprs(n.targ_exprs);
                },
                [&](const ThisExpr&) {},
                [&](const BaseExpr&) {},
                [&](const NullExpr&) {},
                [&](const NumberExpr&) {},
                [&](const StringExpr&) {},
                [&](const CharExpr&) {},
                [&](const MemberExpr& m) { expr(m.obj); },
                [&](const CallExpr& c)
                {
                    expr(c.callee);
                    exprs(c.args);
                },
                [&](const IndexExpr& ix)
                {
                    expr(ix.obj);
                    expr(ix.index);
                },
                [&](const UnaryExpr& u) { expr(u.operand); },
                [&](const BinaryExpr& b)
                {
                    expr(b.lhs);
                    expr(b.rhs);
                },
                [&](const TernaryExpr& t)
                {
                    expr(t.cond);
                    expr(t.a);
                    expr(t.b);
                },
                [&](const ParenExpr& p) { expr(p.inner); },
                [&](const NewExpr& n) { exprs(n.args); },
                [&](const LambdaExpr& l)
                {
                    push();
                    for (const Param& p : l.params)
                        declare(p.name, Kind::Param, l.pos, false);
                    block(l.body);
                    pop();
                },
                [&](const RawExpr&) { opaque_ = true; }, // C++ text: it can use any name
                [&](const TypeTestExpr& tt)
                {
                    expr(tt.obj);
                    declare(tt.bind, Kind::Binding, tt.bind_pos, false);
                },
                [&](const InterpExpr& ip) { exprs(ip.args); },
                [&](const CollectionExpr& co)
                {
                    exprs(co.elems);
                    exprs(co.values);
                },
            },
            e.v);
    }

    // "x = e;" as a statement: the assignment writes "x" and does not read it.
    void expr_statement(const ExprP& e)
    {
        if (e)
            if (auto* b = std::get_if<BinaryExpr>(&e->v); b && b->op == "=" && b->lhs && b->rhs)
                if (auto* n = std::get_if<NameExpr>(&b->lhs->v); n && n->parts.size() == 1)
                {
                    Var* v = find(n->parts[0]);
                    if (v && !v->is_ref)
                    {
                        expr(b->rhs);
                        return;
                    }
                }
        expr(e);
    }

    // ---------------------------------------------------------------- statements
    static const char* exit_word(const Stmt& s)
    {
        if (std::holds_alternative<ReturnStmt>(s.v))
            return "return";
        if (std::holds_alternative<BreakStmt>(s.v))
            return "break";
        if (std::holds_alternative<ContinueStmt>(s.v))
            return "continue";
        if (std::holds_alternative<ThrowStmt>(s.v))
            return "throw";
        return nullptr;
    }

    // Warns once for the first statement that follows an exit statement. "in_switch": a lone
    // "break;" after the exit of a "case" is a usual style, so it gives no warning.
    void dead_code(const std::vector<StmtP>& list, bool in_switch)
    {
        const char* word = nullptr;
        for (const StmtP& s : list)
        {
            if (!s || std::holds_alternative<EmptyStmt>(s->v))
                continue;
            if (word)
            {
                const bool lone_break = in_switch && std::holds_alternative<BreakStmt>(s->v);
                if (!lone_break && stmt_pos(*s) != 0)
                    diag_.warn(stmt_pos(*s), map_,
                        std::string("unreachable code: this statement follows '") + word + "' and never runs");
                return;
            }
            word = exit_word(*s);
        }
    }

    void block(const BlockStmt& b)
    {
        dead_code(b.stmts, false);
        push();
        for (const StmtP& s : b.stmts)
            stmt(s);
        pop();
    }

    void stmt(const StmtP& s)
    {
        if (s)
            stmt(*s);
    }

    // A statement body that is not a block still has its own scope.
    void scoped(const StmtP& s)
    {
        push();
        stmt(s);
        pop();
    }

    // Exhaustive, like "expr": a new kind of statement does not compile until this pass handles it.
    void stmt(const Stmt& s)
    {
        std::visit(
            Overloaded{
                [&](const BlockStmt& b) { block(b); },
                [&](const ExprStmt& es) { expr_statement(es.expr); },
                [&](const VarDeclStmt& d)
                {
                    expr(d.init);
                    if (d.names.empty())
                        declare(d.name, Kind::Local, d.name_pos ? d.name_pos : d.pos, d.is_ref);
                    else
                        for (size_t i = 0; i < d.names.size(); ++i)
                            declare(d.names[i], Kind::Other, i < d.name_poss.size() ? d.name_poss[i] : d.pos, false);
                },
                [&](const IfStmt& i)
                {
                    push(); // the scope of an "is T name" binding: the condition and the "then" branch
                    expr(i.cond);
                    scoped(i.then);
                    pop();
                    scoped(i.otherwise); // "else if (o is B n)" can use the name of the first binding
                },
                [&](const WhileStmt& w)
                {
                    push();
                    expr(w.cond);
                    scoped(w.body);
                    pop();
                },
                [&](const DoWhileStmt& dw)
                {
                    scoped(dw.body);
                    push();
                    expr(dw.cond);
                    pop();
                },
                [&](const ForStmt& f)
                {
                    push();
                    for (const StmtP& x : f.init)
                        stmt(x);
                    expr(f.cond);
                    for (const ExprP& x : f.step)
                        expr_statement(x);
                    scoped(f.body);
                    pop();
                },
                [&](const ForOfStmt& fo)
                {
                    expr(fo.range);
                    push();
                    if (fo.names.empty())
                        declare(fo.name, Kind::Other, fo.name_pos ? fo.name_pos : fo.pos, false);
                    else
                        for (size_t i = 0; i < fo.names.size(); ++i)
                            declare(fo.names[i], Kind::Other, i < fo.name_poss.size() ? fo.name_poss[i] : fo.pos,
                                false);
                    scoped(fo.body);
                    pop();
                },
                [&](const ReturnStmt& r) { expr(r.expr); },
                [&](const BreakStmt&) {},
                [&](const ContinueStmt&) {},
                [&](const SwitchStmt& sw)
                {
                    expr(sw.subject);
                    push(); // all the cases share one scope, as in C++
                    for (const SwitchCase& c : sw.cases)
                    {
                        exprs(c.labels);
                        dead_code(c.stmts, true);
                        for (const StmtP& x : c.stmts)
                            stmt(x);
                    }
                    pop();
                },
                [&](const ThrowStmt& t) { expr(t.expr); },
                [&](const TryStmt& tr)
                {
                    block(tr.block);
                    for (const CatchClause& c : tr.catches)
                    {
                        push();
                        declare(c.name, Kind::Other, c.pos, false);
                        block(c.block);
                        pop();
                    }
                    if (tr.finally_block)
                        block(*tr.finally_block);
                },
                [&](const EmptyStmt&) {},
                [&](const RawStmt&) { opaque_ = true; }, // C++ text: it can use any name
            },
            s.v);
    }
};

} // namespace

void lint_class(const ClassDef& cd, const LineMap& map, Diagnostics& diag)
{
    // Two places are not checked. The base call arguments of a constructor ("base(args)") can use
    // the parameters only, and a parameter never gives a warning. The default value of a parameter
    // is not walked, so a lambda in it is not checked.
    Linter lt(map, diag);
    const size_t first = diag.warnings.size();
    for (const Field& f : cd.fields)
        if (!f.raw)
            lt.init(f.init);
    for (const Method& m : cd.methods)
        if (!m.raw)
            lt.body(m.params, m.body);
    for (const Method& m : cd.statics)
        if (!m.raw)
            lt.body(m.params, m.body);
    for (const Ctor& c : cd.ctors)
        if (!c.raw)
            lt.body(c.params, c.body);
    for (const Method& m : cd.dtors)
        if (!m.raw)
            lt.body(m.params, m.body);
    for (const Property& p : cd.props)
    {
        lt.init(p.init);
        if (p.get_body)
            lt.body({}, *p.get_body);
        if (p.set_body)
            lt.body({}, *p.set_body, "value");
    }
    // The warnings of the pass come in the order of the script text.
    std::stable_sort(diag.warnings.begin() + static_cast<std::ptrdiff_t>(first), diag.warnings.end(),
        [](const Diag& a, const Diag& b) { return a.pos < b.pos; });
}

} // namespace hotc
