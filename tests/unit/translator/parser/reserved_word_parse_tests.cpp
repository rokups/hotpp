// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests for the reserved words "extern", "ref" and "out".
#include "parser/parser.h"
#include "parser/statement.h"
#include "support/statement_parse_test_support.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace hotc;
using namespace stmt_test;

namespace
{

// Parses the script and gives the text of the errors.
std::string errors_of_script(const std::string& text)
{
    const LineMap map(text);
    Diagnostics diag;
    parse_script(text, map, diag);
    return diag.format("t");
}

} // namespace

TEST(ReservedWordParse, ExternStatementIsAnError)
{
    for (const char* text : {"extern int x;", "extern x = 1;"})
    {
        Parsed p(text);
        ASSERT_FALSE(p.diag.items.empty()) << text;
        EXPECT_EQ(p.diag.items[0].msg, "\"extern\" is a reserved word") << text;
    }
}

TEST(ReservedWordParse, ReservedLocalNamesAreErrors)
{
    for (const char* text : {"var ref = 1;", "var out = 1;", "var extern = 1;", "int out = 1;", "var true = 1;"})
    {
        Parsed p(text);
        EXPECT_FALSE(p.diag.items.empty()) << text;
    }
}

TEST(ReservedWordParse, SimilarLocalNamesAreValid)
{
    for (const char* text : {"var refx = 1;", "var outer = 1;", "var external = 1;", "int ref_a = outer + refx;"})
    {
        Parsed p(text);
        EXPECT_TRUE(p.diag.items.empty()) << text << ": " << p.diag.format("t");
    }
}

TEST(ReservedWordParse, ReservedMemberNamesAreErrors)
{
    for (const char* name : {"ref", "out", "extern"})
    {
        const std::string w = name;
        EXPECT_NE(errors_of_script("class A {\n    int " + w + ";\n}\n").find("'" + w + "' is a reserved word"),
            std::string::npos)
            << w;
        EXPECT_FALSE(errors_of_script("class A {\n    void " + w + "() { }\n}\n").empty()) << w;
        EXPECT_FALSE(errors_of_script("class A {\n    void f(int " + w + ") { }\n}\n").empty()) << w;
        EXPECT_FALSE(errors_of_script("class " + w + " {\n}\n").empty()) << w;
    }
}

TEST(ReservedWordParse, SimilarMemberNamesAreValid)
{
    EXPECT_EQ(errors_of_script("class refx {\n"
                               "    int outer;\n"
                               "    int external = 1;\n"
                               "    void refx(int outer, int external) { var ref_a = outer; }\n"
                               "}\n"),
        "");
}
