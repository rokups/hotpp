// Tests for the printer of "ref" and "out".
#include <gtest/gtest.h>

#include <string>

#include "emit.h"
#include "support/emit_test_support.h"

using namespace sppc::ast;
using namespace emit_test;

TEST(RefOutEmit, RefArgument)
{
    EXPECT_EQ(print(*make_expr(RefExpr{id("source"), false})), "spp::ref_arg(source)");
    EXPECT_EQ(print(*make_expr(RefExpr{id("v"), true})), "spp::ref_arg(v)");
}

TEST(RefOutEmit, RefLocal)
{
    VarDeclStmt d;
    d.name = "r";
    d.init = make_expr(RefExpr{dot(id("a"), "b"), false});
    EXPECT_EQ(print(*make_stmt(std::move(d))), "auto& r = a.b;");
    VarDeclStmt t;
    t.name = "r";
    t.type = "Vec3";
    t.init = make_expr(RefExpr{id("p"), false});
    EXPECT_EQ(print(*make_stmt(std::move(t))), "Vec3& r = p;");
    VarDeclStmt c;
    c.is_const = true;
    c.name = "r";
    c.init = make_expr(RefExpr{id("p"), false});
    EXPECT_EQ(print(*make_stmt(std::move(c))), "const auto& r = p;");
}
