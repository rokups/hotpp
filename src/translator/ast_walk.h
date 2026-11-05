// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// A read-only walk over the script AST. The checker and the const inference share it.
#pragma once

#include "ast.h"

#include <functional>
#include <variant>

namespace hotc::walk
{

using namespace ast;

// Visits the nodes in the order of the script text, parent first. The walk goes into the
// bodies of lambdas. "on_expr" and "on_stmt" are called for each node.
template <class FE, class FS> struct Walker
{
    FE& on_expr;
    FS& on_stmt;
    std::function<void()> lambda_end; // optional: called after the body of each lambda
    std::function<void()> block_begin; // optional: called before the statements of each block
    std::function<void()> block_end; // optional: called after the statements of each block
    std::function<void(const Stmt&)> stmt_end; // optional: called after a statement and its children

    void expr(const ExprP& e)
    {
        if (e)
            expr(*e);
    }

    void expr(const Expr& e)
    {
        on_expr(e);
        if (auto* m = std::get_if<MemberExpr>(&e.v))
        {
            expr(m->obj);
        }
        else if (auto* c = std::get_if<CallExpr>(&e.v))
        {
            expr(c->callee);
            for (const ExprP& a : c->args)
                expr(a);
        }
        else if (auto* ix = std::get_if<IndexExpr>(&e.v))
        {
            expr(ix->obj);
            expr(ix->index);
        }
        else if (auto* u = std::get_if<UnaryExpr>(&e.v))
        {
            expr(u->operand);
        }
        else if (auto* b = std::get_if<BinaryExpr>(&e.v))
        {
            expr(b->lhs);
            expr(b->rhs);
        }
        else if (auto* t = std::get_if<TernaryExpr>(&e.v))
        {
            expr(t->cond);
            expr(t->a);
            expr(t->b);
        }
        else if (auto* p = std::get_if<ParenExpr>(&e.v))
        {
            expr(p->inner);
        }
        else if (auto* n = std::get_if<NewExpr>(&e.v))
        {
            for (const ExprP& a : n->args)
                expr(a);
        }
        else if (auto* tt = std::get_if<TypeTestExpr>(&e.v))
        {
            expr(tt->obj);
        }
        else if (auto* ip = std::get_if<InterpExpr>(&e.v))
        {
            for (const ExprP& a : ip->args)
                expr(a);
        }
        else if (auto* co = std::get_if<CollectionExpr>(&e.v))
        {
            for (const ExprP& a : co->elems)
                expr(a);
            for (const ExprP& a : co->values)
                expr(a);
        }
        else if (auto* l = std::get_if<LambdaExpr>(&e.v))
        {
            block(l->body);
            if (lambda_end)
                lambda_end();
        }
    }

    void block(const BlockStmt& b)
    {
        if (block_begin)
            block_begin();
        for (const StmtP& s : b.stmts)
            stmt(s);
        if (block_end)
            block_end();
    }

    void stmt(const StmtP& s)
    {
        if (s)
            stmt(*s);
    }

    void stmt(const Stmt& s)
    {
        on_stmt(s);
        if (auto* b = std::get_if<BlockStmt>(&s.v))
        {
            block(*b);
        }
        else if (auto* es = std::get_if<ExprStmt>(&s.v))
        {
            expr(es->expr);
        }
        else if (auto* d = std::get_if<VarDeclStmt>(&s.v))
        {
            expr(d->init);
        }
        else if (auto* i = std::get_if<IfStmt>(&s.v))
        {
            expr(i->cond);
            stmt(i->then);
            stmt(i->otherwise);
        }
        else if (auto* w = std::get_if<WhileStmt>(&s.v))
        {
            expr(w->cond);
            stmt(w->body);
        }
        else if (auto* dw = std::get_if<DoWhileStmt>(&s.v))
        {
            stmt(dw->body);
            expr(dw->cond);
        }
        else if (auto* f = std::get_if<ForStmt>(&s.v))
        {
            for (const StmtP& x : f->init)
                stmt(x);
            expr(f->cond);
            for (const ExprP& x : f->step)
                expr(x);
            stmt(f->body);
        }
        else if (auto* fo = std::get_if<ForOfStmt>(&s.v))
        {
            expr(fo->range);
            stmt(fo->body);
        }
        else if (auto* r = std::get_if<ReturnStmt>(&s.v))
        {
            expr(r->expr);
        }
        else if (auto* sw = std::get_if<SwitchStmt>(&s.v))
        {
            expr(sw->subject);
            for (const SwitchCase& c : sw->cases)
            {
                for (const ExprP& l : c.labels)
                    expr(l);
                for (const StmtP& x : c.stmts)
                    stmt(x);
            }
        }
        else if (auto* t = std::get_if<ThrowStmt>(&s.v))
        {
            expr(t->expr);
        }
        else if (auto* tr = std::get_if<TryStmt>(&s.v))
        {
            block(tr->block);
            for (const CatchClause& c : tr->catches)
                block(c.block);
            if (tr->finally_block)
                block(*tr->finally_block);
        }
        if (stmt_end)
            stmt_end(s);
    }
};

template <class FE, class FS> Walker<FE, FS> make_walker(FE& fe, FS& fs)
{
    return Walker<FE, FS>{fe, fs, nullptr};
}

} // namespace hotc::walk
