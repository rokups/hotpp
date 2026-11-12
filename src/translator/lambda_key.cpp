// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

#include "lambda_key.h"

#include "ast_walk.h"

#include <map>
#include <set>
#include <variant>

namespace hotc
{

namespace
{

using namespace ast;

struct KeyPass
{
    const std::string& base;
    const LineMap* map;
    Diagnostics& diag;
    std::vector<std::string>* all;

    std::map<const LambdaExpr*, std::string> variable_of; // the variable that a lambda is assigned to
    std::vector<std::string> parent_parts; // the parts of the lambdas around the current one
    std::set<std::string> keys; // all keys of the function, with the alias keys
    std::set<std::string> variable_keys;
    int ordinal = 0;

    void error(uint32_t pos, const std::string& msg)
    {
        if (map)
            diag.add(pos, *map, msg);
        else
            diag.add_line(0, msg);
    }

    static const LambdaExpr* lambda_of(const ExprP& e)
    {
        return e ? std::get_if<LambdaExpr>(&e->v) : nullptr;
    }

    // The variable of the left side of an assignment: "x" or "this.x". Empty: another form.
    static std::string target_of(const ExprP& e)
    {
        if (!e)
            return {};
        if (const auto* n = std::get_if<NameExpr>(&e->v))
        {
            if (n->parts.size() == 1)
                return n->parts[0];
            if (n->parts.size() == 2 && n->parts[0] == "this")
                return n->parts[1];
        }
        else if (const auto* m = std::get_if<MemberExpr>(&e->v))
        {
            if (m->obj && std::holds_alternative<ThisExpr>(m->obj->v) && !m->safe)
                return m->name;
        }
        return {};
    }

    std::string prefix() const
    {
        std::string r = base;
        for (const std::string& p : parent_parts)
            r += "/" + p;
        return r;
    }

    void visit_expr(const Expr& e)
    {
        if (const auto* b = std::get_if<BinaryExpr>(&e.v))
        {
            if (b->op == "=")
                if (const LambdaExpr* l = lambda_of(b->rhs))
                {
                    const std::string v = target_of(b->lhs);
                    if (!v.empty())
                        variable_of[l] = v;
                }
        }
        const auto* l = std::get_if<LambdaExpr>(&e.v);
        if (!l)
            return;
        LambdaExpr& m = const_cast<LambdaExpr&>(*l);
        std::string part;
        const auto var = variable_of.find(l);
        if (!m.name.empty())
            part = "n:" + m.name;
        else if (var != variable_of.end())
            part = "v:" + var->second;
        else
            part = "#" + std::to_string(ordinal++);
        m.key = prefix() + "/" + part;
        m.alias_keys.clear();
        if (!m.name.empty())
        {
            if (!keys.insert(m.key).second)
                error(m.name_pos, "two lambdas of this function have the name '" + m.name + "'");
        }
        else if (var != variable_of.end())
        {
            if (!keys.insert(m.key).second)
                error(m.pos, "two lambdas of this function are assigned to '" + var->second
                                 + "'. Add [name(\"...\")] to each lambda");
        }
        else
        {
            keys.insert(m.key);
        }
        if (all)
            all->push_back(m.key);
        if (!m.aliases.empty() && m.name.empty())
            error(m.name_pos ? m.name_pos : m.pos, "[alias(\"" + m.aliases[0] + "\")] needs [name(\"...\")] on the lambda");
        else
            for (const std::string& a : m.aliases)
            {
                const std::string k = prefix() + "/n:" + a;
                if (!keys.insert(k).second)
                    error(m.name_pos ? m.name_pos : m.pos,
                        "the alias '" + a + "' is already the key of another lambda of this function");
                m.alias_keys.push_back(k);
                if (all)
                    all->push_back(k);
            }
        parent_parts.push_back(part);
    }

    void visit_stmt(const Stmt& s)
    {
        if (const auto* d = std::get_if<VarDeclStmt>(&s.v))
            if (const LambdaExpr* l = lambda_of(d->init))
                variable_of[l] = d->name;
    }
};

} // namespace

void assign_lambda_keys(
    ast::BlockStmt& body, const std::string& base, const LineMap* map, Diagnostics& diag, std::vector<std::string>* all)
{
    KeyPass pass{base, map, diag, all, {}, {}, {}, {}, 0};
    auto on_expr = [&](const Expr& e) { pass.visit_expr(e); };
    auto on_stmt = [&](const Stmt& s) { pass.visit_stmt(s); };
    auto walker = walk::make_walker(on_expr, on_stmt);
    walker.lambda_end = [&] { pass.parent_parts.pop_back(); };
    walker.block(body);
}

} // namespace hotc
