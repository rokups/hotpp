// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests for the errors of the lowering pass: the line, the text and the recovery.
#include "emit.h"
#include "lower.h"
#include "parser/expression.h"
#include "parser/parser.h"
#include "parser/statement.h"
#include "support/lowering_test_support.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace hotc;
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

TEST(LoweringError, CollectionLiteralNeedsADeclaredType)
{
    Fixture f;
    f.body("var xs = [1, 2];\nf([3]);\nx = [4];");
    ASSERT_EQ(f.errors().size(), 3u);
    EXPECT_EQ(f.errors()[0], "1:10: a collection literal needs a declared type");
    EXPECT_EQ(f.errors()[1], "2:3: a collection literal needs a declared type");
    EXPECT_EQ(f.errors()[2], "3:5: a collection literal needs a declared type");
}

TEST(LoweringError, StringSwitchCaseMustEndWithAJump)
{
    Fixture f;
    f.body("switch (s) {\ncase \"a\":\nf();\ncase \"b\":\ng();\nbreak;\n}");
    ASSERT_EQ(f.errors().size(), 1u);
    EXPECT_EQ(f.errors()[0], "2:1: a string switch case must end with break, return, throw or continue");
}

TEST(LoweringError, StringSwitchCannotMixLabels)
{
    Fixture f;
    f.body("switch (s) {\ncase \"a\":\nf();\nbreak;\ncase 1:\ng();\nbreak;\n}");
    ASSERT_EQ(f.errors().size(), 1u);
    EXPECT_EQ(f.errors()[0], "5:1: a switch cannot mix string labels and other labels");
}

TEST(LoweringError, StringSwitchDuplicateLabel)
{
    Fixture f;
    f.body("switch (s) {\ncase \"a\":\nbreak;\ncase \"a\":\nbreak;\n}");
    ASSERT_EQ(f.errors().size(), 1u);
    EXPECT_EQ(f.errors()[0], "4:1: duplicate case label \"a\"");
}

TEST(LoweringError, StringSwitchNestedBreakAndContinue)
{
    Fixture f;
    f.body("while (x) { switch (s) {\ncase \"a\":\nif (y) { break; }\ncontinue;\n} }");
    ASSERT_EQ(f.errors().size(), 1u);
    EXPECT_EQ(f.errors()[0], "1:13: a string switch cannot have both a nested 'break' and a 'continue'");
}
