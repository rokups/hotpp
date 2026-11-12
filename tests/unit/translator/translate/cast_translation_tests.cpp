// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests for the casts of hotc::translate. A ".hot" script has no named casts: a named cast is an
// error. A C style cast, a functional cast, "as" and "is" stay valid. A ".cpp" script keeps the
// named casts. The unit name is "t".
#include "hot/hotc_lib.h"
#include "support/translator_test_support.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>

using namespace translator_test;

namespace
{

const char* kHeader = "class Widget { public: virtual ~Widget() {} virtual void Update(float dt) {} };\n"
                      "class Nat : public Widget { };\n"
                      "enum class NativeMode { A, B };\n";

hotc::TranslateResult run(const std::string& script)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kHeader);
    return hotc::translate(temp_options(dir, "using \"w.h\";\n" + script));
}

const std::string kStaticHint = "is not supported: use a C style cast '(T)e' or 'T(e)'";

// The script must fail with "t.hot:<line>:<col>: error: '<name>' <hint>".
void expect_cast_error(const std::string& script, const std::string& pos, const std::string& name,
    const std::string& hint)
{
    const hotc::TranslateResult r = run(script);
    EXPECT_FALSE(r.ok) << script;
    const std::string text = "t.hot:" + pos + ": error: '" + name + "' " + hint;
    EXPECT_NE(r.diagnostics.find(text), std::string::npos) << "expected: " << text << "\n" << r.diagnostics;
}

// The same, with the position of the first "static_cast" in the script. The "using" line adds one line.
void expect_static_cast_error(const std::string& script)
{
    const size_t at = script.find("static_cast");
    ASSERT_NE(at, std::string::npos);
    const size_t line_start = script.rfind('\n', at) == std::string::npos ? 0 : script.rfind('\n', at) + 1;
    const size_t line = 2 + std::count(script.begin(), script.begin() + static_cast<std::ptrdiff_t>(at), '\n');
    expect_cast_error(script, std::to_string(line) + ":" + std::to_string(at - line_start + 1), "static_cast",
        kStaticHint);
}

void expect_ok(const std::string& script)
{
    const hotc::TranslateResult r = run(script);
    EXPECT_TRUE(r.ok) << script << "\n" << r.diagnostics;
    EXPECT_EQ(r.diagnostics.find("is not supported"), std::string::npos) << r.diagnostics;
}

} // namespace

TEST(CastTranslation, StaticCastInAMethodIsAnError)
{
    expect_cast_error("class Foo : Widget {\n    int F(float f) { return static_cast<int>(f); }\n}\n", "3:29",
        "static_cast", kStaticHint);
}

TEST(CastTranslation, EachNamedCastIsAnError)
{
    expect_cast_error("class Foo : Widget {\n    void F(Widget* p) { var q = dynamic_cast<Nat*>(p); }\n}\n", "3:33",
        "dynamic_cast", "is not supported: use 'as' or 'is'");
    expect_cast_error("class Foo : Widget {\n    void F(Widget* p) { var q = const_cast<Nat*>(p); }\n}\n", "3:33",
        "const_cast", kStaticHint);
    expect_cast_error("class Foo : Widget {\n    void F(Widget* p) { var q = reinterpret_cast<Nat*>(p); }\n}\n", "3:33",
        "reinterpret_cast", kStaticHint);
}

TEST(CastTranslation, ANamedCastInAFieldInitializerIsAnError)
{
    expect_cast_error("class Foo : Widget {\n    int n = static_cast<int>(2.5);\n}\n", "3:13", "static_cast", kStaticHint);
}

TEST(CastTranslation, ANamedCastInAModuleFunctionIsAnError)
{
    expect_cast_error("int Half(float f) { return static_cast<int>(f / 2.0); }\n", "2:28", "static_cast", kStaticHint);
}

TEST(CastTranslation, ANamedCastInALambdaIsAnError)
{
    expect_cast_error("class Foo : Widget {\n    void F() {\n        var g = (float f) => static_cast<int>(f);\n"
                      "        int x = g(1.5);\n    }\n}\n",
        "4:30", "static_cast", kStaticHint);
}

TEST(CastTranslation, ANamedCastInAnInterpolatedStringIsAnError)
{
    expect_cast_error("class Foo : Widget {\n    float f = 1.5;\n    void F() { std.string s = \"v ${static_cast<int>(this.f)}\"; }\n}\n",
        "4:36", "static_cast", kStaticHint);
}

TEST(CastTranslation, ANamedCastInAMethodDefaultArgumentIsAnError)
{
    expect_static_cast_error("class Foo : Widget {\n    void F(int x = static_cast<int>(2.5)) { }\n}\n");
}

TEST(CastTranslation, ANamedCastInAStaticMethodDefaultArgumentIsAnError)
{
    expect_static_cast_error("class Foo : Widget {\n    static void F(int x = static_cast<int>(2.5)) { }\n}\n");
}

TEST(CastTranslation, ANamedCastInAConstructorDefaultArgumentIsAnError)
{
    expect_static_cast_error("class Foo : Widget {\n    Foo(int x = static_cast<int>(2.5)) { }\n}\n");
}

TEST(CastTranslation, ANamedCastInAModuleFunctionDefaultArgumentIsAnError)
{
    expect_static_cast_error("void F(int x = static_cast<int>(2.5)) { }\n");
}

TEST(CastTranslation, ANamedCastInABaseCallIsAnError)
{
    expect_static_cast_error("class A { A(int a) { } }\nclass B : A {\n    B(float f) : base(static_cast<int>(f)) { }\n}\n");
}

TEST(CastTranslation, ANamedCastInAModuleVariableIsAnError)
{
    expect_static_cast_error("int g = static_cast<int>(2.5);\n");
}

TEST(CastTranslation, ANamedCastInAModuleConstIsAnError)
{
    expect_static_cast_error("const int K = static_cast<int>(2.5);\n");
}

TEST(CastTranslation, ANamedCastInAClassConstIsAnError)
{
    expect_static_cast_error("class Foo : Widget {\n    static const int K = static_cast<int>(2.5);\n}\n");
}

TEST(CastTranslation, ANamedCastInAStaticFieldIsAnError)
{
    expect_static_cast_error("class Foo : Widget {\n    static int s = static_cast<int>(2.5);\n}\n");
}

TEST(CastTranslation, ANamedCastInAPropertyInitializerIsAnError)
{
    expect_static_cast_error("class Foo : Widget {\n    int P { get; set; } = static_cast<int>(2.5);\n}\n");
}

TEST(CastTranslation, ANamedCastInAPropertyAccessorIsAnError)
{
    expect_static_cast_error("class Foo : Widget {\n    int P { get { return static_cast<int>(2.5); } }\n}\n");
}

TEST(CastTranslation, ANamedCastInAnEnumValueIsAnError)
{
    expect_static_cast_error("enum E { A = static_cast<int>(2.5) }\n");
}

TEST(CastTranslation, ANamedCastInAStaticAssertIsAnError)
{
    expect_static_cast_error("static_assert(static_cast<int>(2.5) == 2, \"x\");\n");
}

TEST(CastTranslation, CStyleAndFunctionalCastsStayValid)
{
    expect_ok("enum Mode { Idle, Walk }\n"
              "class Foo : Widget {\n"
              "    float f = 1.5;\n"
              "    void F(Widget* p, $Foo h, Widget* w, int i) {\n"
              "        int a = (int)this.f;\n"
              "        int b = int(this.f);\n"
              "        Nat* c = (Nat*)p;\n"
              "        $Foo d = ($Foo)h;\n"
              "        var e = w as $Foo;\n"
              "        bool g = w is $Foo;\n"
              "        Mode m = Mode(i);\n"
              "        NativeMode n = NativeMode(i);\n"
              "    }\n"
              "}\n");
}

TEST(CastTranslation, AnEnumInAnInterpolatedStringStaysValid)
{
    // The generated C++ of the enum text has a "static_cast". The check must not see it.
    expect_ok("enum Mode { Idle, Walk }\n"
              "class Foo : Widget {\n"
              "    Mode m = Mode.Walk;\n"
              "    void F() { std.string s = \"m ${this.m}\"; }\n"
              "}\n");
}

TEST(CastTranslation, ANativeEnumClassValueInAnInterpolatedStringNeedsACStyleCast)
{
    expect_ok("class Foo : Widget {\n"
              "    void F(NativeMode m) { std.string s = \"m ${(int)m}\"; }\n"
              "}\n");
}

TEST(CastTranslation, ACppScriptKeepsTheNamedCasts)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kHeader);
    hotc::TranslateOptions o = temp_options(dir, "#include \"w.h\"\n"
                                                 "class A : public Widget { public:\n"
                                                 "    int F(float f) { return static_cast<int>(f); }\n"
                                                 "    A* G(Widget* w) { return dynamic_cast<A*>(w); }\n"
                                                 "};\n");
    o.script_name = "t.cpp";
    const hotc::TranslateResult r = hotc::translate(o);
    EXPECT_TRUE(r.ok) << r.diagnostics;
}
