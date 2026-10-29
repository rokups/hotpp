// Helpers for the tests of the printer. They build the nodes of the tree by hand.
#pragma once

#include <gtest/gtest.h>
#include <initializer_list>
#include <string>
#include <vector>

#include "emit.h"

namespace emit_test {

using namespace sppc::ast;

inline ExprP name(std::initializer_list<std::string> parts, uint32_t pos = 0)
{
    NameExpr n;
    n.parts = parts;
    n.pos = pos;
    return make_expr(std::move(n));
}
inline ExprP id(const std::string& s, uint32_t pos = 0) { return name({s}, pos); }
inline ExprP num(const std::string& s, uint32_t pos = 0) { return make_expr(NumberExpr{s, pos}); }
inline ExprP str(const std::string& s) { return make_expr(StringExpr{s}); }
// The lowering pass writes "__st->x" as raw text. This helper makes the same node.
inline ExprP arrow(ExprP obj, const std::string& m)
{
    const uint32_t pos = pos_of(*obj);
    return make_expr(RawExpr{print(*obj) + "->" + m, pos});
}
inline ExprP dot(ExprP obj, const std::string& m) { return make_expr(MemberExpr{std::move(obj), m}); }
inline ExprP bin(const std::string& op, ExprP l, ExprP r)
{
    return make_expr(BinaryExpr{op, std::move(l), std::move(r)});
}
inline ExprP un(const std::string& op, ExprP x, bool postfix = false)
{
    return make_expr(UnaryExpr{op, std::move(x), postfix});
}
inline ExprP call(ExprP callee, std::vector<ExprP> args)
{
    return make_expr(CallExpr{std::move(callee), std::move(args)});
}
template <class... A>
std::vector<ExprP> list(A... a)
{
    std::vector<ExprP> v;
    (v.push_back(std::move(a)), ...);
    return v;
}
template <class... A>
std::vector<StmtP> slist(A... a)
{
    std::vector<StmtP> v;
    (v.push_back(std::move(a)), ...);
    return v;
}
inline StmtP es(ExprP x) { return make_stmt(ExprStmt{std::move(x)}); }
inline StmtP blk(std::vector<StmtP> v)
{
    BlockStmt b;
    b.stmts = std::move(v);
    return make_stmt(std::move(b));
}

}  // namespace emit_test
