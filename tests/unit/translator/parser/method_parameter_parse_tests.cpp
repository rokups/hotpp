// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests for the parser of the method parameters: type first, "ref", "out" and errors.
#include "parser/parser.h"
#include "support/script_parse_test_support.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace hotc;
using namespace script_test;

TEST(MethodParameterParse, TypeFirstParameters)
{
    Diagnostics sd;
    EXPECT_EQ(method_params("class W : B {\n"
                            "    void f(Actor* a, std.string s, in T r,\n"
                            "               std.map<int, std.vector<float>> m) { }\n"
                            "}\n",
                  sd),
        (std::vector<std::string>{
            "Actor*|a", "\x01std::string|s", "const T&|r", "\x01std::map<int32_t, \x01std::vector<float>>|m"}));
    EXPECT_EQ(sd.items.size(), 0u) << sd.format("t");
}

TEST(MethodParameterParse, ReferenceParameters)
{
    const std::string text =
        "class W : B {\n"
        "    void f(ref Vec3 a, ref Actor b, int c) { ref Vec3 r = a; Fill(b); }\n"
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

TEST(MethodParameterParse, ModifierSetsTheModeAndTheCppType)
{
    const std::string text = "class W : B {\n"
                             "    void f(in T a, ref T b, out T c, [byval] T d, T e) { }\n"
                             "}\n";
    Diagnostics sd;
    const LineMap map(text);
    const ast::Script script = parse_script(text, map, sd);
    ASSERT_EQ(sd.items.size(), 0u) << sd.format("t");
    const auto& ps = script.classes.at(0).methods.at(0).params;
    ASSERT_EQ(ps.size(), 5u);
    const ast::ParamMode modes[] = {ast::ParamMode::In, ast::ParamMode::Ref, ast::ParamMode::Out,
        ast::ParamMode::ByVal, ast::ParamMode::Plain};
    const char* types[] = {"const T&", "T&", "T&", "T", "T"};
    for (size_t i = 0; i < 5; ++i)
    {
        EXPECT_EQ(ps[i].mode, modes[i]) << i;
        EXPECT_EQ(ps[i].type, types[i]) << i;
    }
}

TEST(MethodParameterParse, AmpersandInATypeIsAnError)
{
    Diagnostics sd;
    method_params("class W : B {\n    void f(Vec3& a) { }\n}\n", sd);
    EXPECT_NE(sd.format("t").find("\"&\" is not a type suffix; write ref T x"), std::string::npos) << sd.format("t");
}
