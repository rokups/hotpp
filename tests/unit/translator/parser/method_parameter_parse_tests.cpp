// Tests for the parser of the method parameters: type first, "ref", "out" and errors.
#include "parser/parser.h"
#include "support/script_parse_test_support.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace sppc;
using namespace script_test;

TEST(MethodParameterParse, TypeFirstParameters)
{
    Diagnostics sd;
    EXPECT_EQ(method_params("class W : B {\n"
                            "    void f(Actor* a, std.string s, const T& r,\n"
                            "               std.map<int, std.vector<float>> m) { }\n"
                            "}\n",
                  sd),
        (std::vector<std::string>{
            "Actor*|a", "std::string|s", "const T&|r", "std::map<int32_t, std::vector<float>>|m"}));
    EXPECT_EQ(sd.items.size(), 0u) << sd.format("t");
}

TEST(MethodParameterParse, ReferenceParameters)
{
    const std::string text =
        "class W : B {\n"
        "    void f(Vec3& a, Actor& b, int c) { Vec3& r = a; Fill(b); }\n"
        "}\n";
    Diagnostics sd;
    const LineMap map(text);
    const ast::Script script = parse_script(text, map, sd);
    ASSERT_EQ(sd.items.size(), 0u) << sd.format("t");
    ASSERT_EQ(script.classes.size(), 1u);
    ASSERT_EQ(script.classes[0].methods.size(), 1u);
    const auto& ps = script.classes[0].methods[0].params;
    ASSERT_EQ(ps.size(), 3u);
    EXPECT_EQ(ps[0].type, "Vec3&");
    EXPECT_TRUE(ast::is_ref_type(ps[0].type));
    EXPECT_EQ(ps[1].type, "Actor&");
    EXPECT_FALSE(ast::is_ref_type(ps[2].type));
    EXPECT_EQ(script.classes[0].methods[0].body.stmts.size(), 2u);
}

TEST(MethodParameterParse, ParameterErrors)
{
    {
        // A method parameter needs a type.
        Diagnostics sd;
        method_params("class W : B {\n    void f(dt) { }\n}\n", sd);
        EXPECT_NE(sd.format("t").find("a parameter needs a type; write 'Type name'"), std::string::npos)
            << sd.format("t");
    }
}
