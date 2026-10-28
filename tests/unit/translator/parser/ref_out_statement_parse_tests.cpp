// Tests for "ref" in a local declaration and in a call statement, and for the removed "&".
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "emit.h"
#include "parser/expression.h"
#include "parser/parser.h"
#include "parser/statement.h"
#include "support/statement_parse_test_support.h"

using namespace spc;
using namespace stmt_test;

TEST(RefOutStatementParse, RefLocalAndRefArguments)
{
    EXPECT_EQ(round_trip("var r = ref this.pos;"), "auto& r = this.pos;");
    EXPECT_EQ(round_trip("Vec3 r = ref p;"), "Vec3& r = p;");
    EXPECT_EQ(round_trip("const var r = ref p;"), "const auto& r = p;");
    EXPECT_EQ(round_trip("Fill(out v); Swap(ref a, ref b);"),
              "Fill(script::ref_arg(v)); Swap(script::ref_arg(a), script::ref_arg(b));");
    EXPECT_EQ(round_trip("this.target = ref source;"), "this.target = script::ref_arg(source);");
}

TEST(RefOutStatementParse, PrefixAmpersandIsRemoved)
{
    Parsed p("var r = &x;");
    EXPECT_EQ(errors_of(p), std::vector<std::string>{"1:9: use 'ref' instead of '&'"});
    Parsed q("f(&x);");
    EXPECT_EQ(errors_of(q), std::vector<std::string>{"1:3: use 'ref' instead of '&'"});
}
