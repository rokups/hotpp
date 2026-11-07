// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Translation tests of the builtin calls of gap G08: "sizeof(T)", "assert(cond)",
// "assert(cond, "message")" and "static_assert(cond, "message")".
#include "hot/hotc_lib.h"
#include "support/translator_test_support.h"

#include <gtest/gtest.h>

#include <string>

namespace
{

using namespace translator_test;

const char* kHeader = "#include <cassert>\n"
                      "struct Header { int a; int b; int c; };\n";

hotc::TranslateResult translate_script(const TempDir& dir, const std::string& script)
{
    write_file(dir.path() / "w.h", kHeader);
    return hotc::translate(temp_options(dir, "using \"w.h\";\n" + script));
}

bool has(const std::string& text, const char* part)
{
    return text.find(part) != std::string::npos;
}

// "sizeof" of a native type, a builtin type and a fixed-layout struct is the C++ "sizeof" of the
// C++ type. The translator does not compute the value.
TEST(BuiltinCallTranslation, SizeofTypes)
{
    TempDir dir;
    const hotc::TranslateResult r = translate_script(dir,
        "struct Inner { int a; float b; }\n"
        "struct Wire { Inner i; uint c; Header h; }\n"
        "nuint F() { nuint a = sizeof(Header); var b = sizeof(int); return a + b + sizeof(Wire) + sizeof(byte); }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "sizeof(Header)")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "sizeof(int32_t)")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "sizeof(Wire)")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "sizeof(uint8_t)")) << r.cpp;
}

// "sizeof" of a script class is an error: a reload can change the layout of the state. A value is
// not a type.
TEST(BuiltinCallTranslation, SizeofErrors)
{
    TempDir dir;
    hotc::TranslateResult r = translate_script(dir, "class C { int a; }\nnuint F() { return sizeof(C); }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "'sizeof' of the script class 'C' is not valid")) << r.diagnostics;
    r = translate_script(dir, "nuint F(int v) { return sizeof(v); }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "'sizeof' takes a type, not the value 'v'")) << r.diagnostics;
}

// "assert(cond)" is the C++ "assert". The message form puts the text in the condition.
TEST(BuiltinCallTranslation, Assert)
{
    TempDir dir;
    hotc::TranslateResult r = translate_script(dir, "int F(int v) { assert(v > 0); assert(v < 9, \"small\"); return v; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "assert(v > 0);")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "assert((v < 9) && \"small\");")) << r.cpp;
    r = translate_script(dir, "int F(int v) { assert(v > 0, v); return v; }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "'assert' takes a condition and an optional text literal")) << r.diagnostics;
}

// "static_assert" at file level, in a class body and in a function body. A class item sees the
// class constants.
TEST(BuiltinCallTranslation, StaticAssert)
{
    TempDir dir;
    hotc::TranslateResult r = translate_script(dir,
        "struct P { int x; int y; }\n"
        "static_assert(sizeof(Header) == 12, \"header size\");\n"
        "static_assert(sizeof(P) == 8, \"P size\");\n"
        "class C {\n"
        "    static const int Max = 4;\n"
        "    static_assert(Max == 4, \"max\");\n"
        "    int F() { static_assert(sizeof(int) == 4, \"int\"); return Max; }\n"
        "}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "static_assert(sizeof(Header) == 12, \"header size\");")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "static_assert(sizeof(P) == 8, \"P size\");")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "static_assert(C__Max == 4, \"max\");")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "static_assert(sizeof(int32_t) == 4, \"int\");")) << r.cpp;
    r = translate_script(dir, "static_assert(true);\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "'static_assert' takes a condition and a text literal")) << r.diagnostics;
}

} // namespace
