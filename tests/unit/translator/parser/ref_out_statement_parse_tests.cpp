// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests for the "T&" local, the call statement without a marker, and the removed "ref" and "out".
#include "emit.h"
#include "parser/expression.h"
#include "parser/parser.h"
#include "parser/statement.h"
#include "support/statement_parse_test_support.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace hotc;
using namespace stmt_test;

TEST(RefOutStatementParse, ReferenceLocalAndArguments)
{
    EXPECT_EQ(round_trip("ref Vec3 r = this.pos;"), "Vec3& r = this.pos;");
    EXPECT_EQ(round_trip("ref Vec3 r = p;"), "Vec3& r = p;");
    EXPECT_EQ(round_trip("Fill(v); Swap(a, b);"), "Fill(v); Swap(a, b);");
}

TEST(RefOutStatementParse, RefAndOutAreRemoved)
{
    for (const char* text :
        {"var r = ref this.pos;", "Fill(out v);", "Swap(ref a, ref b);", "this.target = ref source;"})
    {
        Parsed p(text);
        EXPECT_FALSE(p.diag.items.empty()) << text;
    }
}

// The prefix "&" parses; the lowering accepts it only as a call argument (HOT-70).
TEST(RefOutStatementParse, PrefixAmpersandParses)
{
    for (const char* text : {"var r = &x;", "f(&x);", "p = c ? &a : &b;"})
    {
        Parsed p(text);
        EXPECT_TRUE(p.diag.items.empty()) << text << p.diag.format("t");
    }
}
