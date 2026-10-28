// Tests for the errors of the lowering pass: the line, the text and the recovery.
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "emit.h"
#include "lower.h"
#include "parser/expression.h"
#include "parser/parser.h"
#include "parser/statement.h"
#include "support/lowering_test_support.h"

using namespace spc;
using namespace lowering_test;

TEST(LoweringError, ErrorThisInFieldInitializer)
{
    Fixture f;
    f.cx.allow_this = false;
    f.expr("1 + this.n");
    f.expr("this");
    ASSERT_EQ(f.errors().size(), 2u);
    EXPECT_EQ(f.errors()[0], "1:5: 'this' is not allowed in a field initializer");
    EXPECT_EQ(f.errors()[1], "1:1: 'this' is not allowed in a field initializer");
}

TEST(LoweringError, ErrorWithoutMapHasNoLine)
{
    Fixture f;
    f.cx.allow_this = false;
    ast::ExprP e = ast::make_expr(ast::ThisExpr{5});
    lower_expr(e, f.cx, f.diag);
    ASSERT_EQ(f.diag.items.size(), 1u);
    EXPECT_EQ(f.diag.items[0].line, 0);
}

TEST(LoweringError, ErrorsDoNotStopThePass)
{
    Fixture f;
    f.cx.allow_this = false;
    EXPECT_EQ(f.expr("f(this, 1.5)"), "f(this, 1.5f)");
    EXPECT_EQ(f.errors().size(), 1u);
}
