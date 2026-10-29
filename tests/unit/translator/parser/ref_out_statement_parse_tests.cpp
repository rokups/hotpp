// Tests for the "T&" local, the call statement without a marker, and the removed "ref" and "out".
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "emit.h"
#include "parser/expression.h"
#include "parser/parser.h"
#include "parser/statement.h"
#include "support/statement_parse_test_support.h"

using namespace sppc;
using namespace stmt_test;

TEST(RefOutStatementParse, ReferenceLocalAndArguments)
{
    EXPECT_EQ(round_trip("Vec3& r = this.pos;"), "Vec3& r = this.pos;");
    EXPECT_EQ(round_trip("Vec3& r = p;"), "Vec3& r = p;");
    EXPECT_EQ(round_trip("const Vec3& r = p;"), "const Vec3& r = p;");
    EXPECT_EQ(round_trip("Fill(v); Swap(a, b);"), "Fill(v); Swap(a, b);");
}

TEST(RefOutStatementParse, RefAndOutAreRemoved)
{
    for (const char* text : {"var r = ref this.pos;", "Fill(out v);", "Swap(ref a, ref b);",
                             "this.target = ref source;"}) {
        Parsed p(text);
        EXPECT_FALSE(p.diag.items.empty()) << text;
    }
}

TEST(RefOutStatementParse, PrefixAmpersandIsAddressOf)
{
    EXPECT_EQ(round_trip("var r = &x;"), "auto r = &x;");
    EXPECT_EQ(round_trip("f(&x);"), "f(&x);");
}
