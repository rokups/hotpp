// Tests for the parser of the method parameters: type first, "ref", "out" and errors.
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "parser/parser.h"
#include "support/script_parse_test_support.h"

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
              (std::vector<std::string>{"Actor*|a", "std::string|s", "const T&|r",
                                        "std::map<int32_t, std::vector<float>>|m"}));
    EXPECT_EQ(sd.items.size(), 0u) << sd.format("t");
}

TEST(MethodParameterParse, RefAndOutParameters)
{
    const std::string text =
        "class W : B {\n"
        "    void f(ref Vec3 a, out Actor b, int c) { var r = ref a; Fill(out b); }\n"
        "}\n";
    Diagnostics sd;
    const LineMap map(text);
    const ast::Script script = parse_script(text, map, sd);
    ASSERT_EQ(sd.items.size(), 0u) << sd.format("t");
    ASSERT_EQ(script.classes.size(), 1u);
    ASSERT_EQ(script.classes[0].methods.size(), 1u);
    const auto& ps = script.classes[0].methods[0].params;
    ASSERT_EQ(ps.size(), 3u);
    EXPECT_EQ(ps[0].mode, ast::PassMode::Ref);
    EXPECT_EQ(ps[0].type, "Vec3");
    EXPECT_EQ(ps[1].mode, ast::PassMode::Out);
    EXPECT_EQ(ps[1].type, "Actor");
    EXPECT_EQ(ps[2].mode, ast::PassMode::Value);
    EXPECT_EQ(script.classes[0].methods[0].body.stmts.size(), 2u);
}

TEST(MethodParameterParse, RefAndOutAreNotNames)
{
    Diagnostics sd;
    method_params("class W : B {\n    void f(int ref) { }\n}\n", sd);
    EXPECT_FALSE(sd.items.empty());
    Diagnostics sd2;
    method_params("class W : B {\n    void f(int out) { }\n}\n", sd2);
    EXPECT_FALSE(sd2.items.empty());
}

TEST(MethodParameterParse, ParameterErrors)
{
    {
        // The old form "name: type" gives a clear error.
        Diagnostics sd;
        method_params("class W : B {\n    void f(dt: float) { }\n}\n", sd);
        ASSERT_GE(sd.items.size(), 1u);
        EXPECT_NE(sd.format("t").find("t:2:12: error: parameter syntax changed; write 'Type name'"),
                  std::string::npos)
            << sd.format("t");
    }
    {
        // A method parameter needs a type.
        Diagnostics sd;
        method_params("class W : B {\n    void f(dt) { }\n}\n", sd);
        EXPECT_NE(sd.format("t").find("a parameter needs a type; write 'Type name'"),
                  std::string::npos)
            << sd.format("t");
    }
}
