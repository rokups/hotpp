// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests for the method groups of hotc::translate (doc/lambdas.md, "Method groups"): the name
// resolution, the errors, the key text and the generated C++. The unit name is "t".
#include "hot/hotc_lib.h"
#include "support/translator_test_support.h"

#include <gtest/gtest.h>

#include <string>

using namespace translator_test;

namespace
{

// A native base with a template SubscribeToEvent and a holder with a template Subscribe, in the
// form of the engine.
const char* kHeader = "#include <functional>\n"
                      "class Widget { public: virtual ~Widget() {} virtual void Update(float dt) {}\n"
                      "    template <class T> void SubscribeToEvent(int e, T handler) {} };\n"
                      "template <class... A> struct Sig { template <class R, class C> void Subscribe(R* r, C c) {} };\n"
                      "void take(std::function<void(int)> f);\n"
                      "void take_int(int v);\n";

hotc::TranslateResult run(const std::string& script)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kHeader);
    return hotc::translate(temp_options(dir, "using \"w.h\";\n" + script));
}

bool has(const hotc::TranslateResult& r, const std::string& text)
{
    return r.cpp.find(text) != std::string::npos;
}

void expect_error(const std::string& script, const std::string& msg)
{
    const hotc::TranslateResult r = run(script);
    EXPECT_FALSE(r.ok) << script;
    EXPECT_NE(r.diagnostics.find(msg), std::string::npos) << r.diagnostics;
}

const std::string kFoo = "class Foo : Widget {\n    int n = 0;\n    void Hit(int x) { n = n + x; }\n";

} // namespace

TEST(MethodGroupTranslation, BareAndThisFormsAreBoundGroups)
{
    const auto r = run(kFoo + "    void run() { take(Hit); take(this.Hit); }\n}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r, "key = \"t:Foo::run/m:this.Hit\";")) << r.cpp;
    // Two equal parts are legal. The second gets an ordinal.
    EXPECT_TRUE(has(r, "key = \"t:Foo::run/m:this.Hit#1\";")) << r.cpp;
    // The bound group is a lambda box with the parameter types of the method.
    EXPECT_TRUE(has(r, "hot::make_lambda_deduced<__LamTag_0>(GEN, hot::caps(__self), []([[maybe_unused]] auto& __c, int32_t __mg0)"))
        << r.cpp;
}

TEST(MethodGroupTranslation, EngineFormsCompileToTheBoxAndToAMethodRef)
{
    const auto r = run(kFoo + "    void run(ref Sig<int> s) {\n        s.Subscribe(this, Hit);\n"
                              "        s.Subscribe(this, Foo.Hit);\n        SubscribeToEvent(1, Hit);\n    }\n}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r, "key = \"t:Foo::run/m:this.Hit\";")) << r.cpp;
    EXPECT_TRUE(has(r, "key = \"t:Foo::run/m:this.Hit#1\";")) << r.cpp;
    EXPECT_TRUE(has(r, "hot::MethodRef<void(Widget*, int32_t), Foo_Shell, SLOT_Hit>(GEN)")) << r.cpp;
}

TEST(MethodGroupTranslation, GroupOnALocalCapturesTheLocal)
{
    const auto r = run(kFoo + "    void run($Foo o) { if (o != null) { take(o.Hit); } }\n}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r, "key = \"t:Foo::run/m:o.Hit\";")) << r.cpp;
    EXPECT_TRUE(has(r, "hot::caps(__self, o)")) << r.cpp;
}

TEST(MethodGroupTranslation, GroupInALambdaHasTheKeyOfTheLambdaAsPrefix)
{
    const auto r = run(kFoo + "    void run() { var f = () => { take(Hit); }; f(); }\n}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r, "key = \"t:Foo::run/v:f/m:this.Hit\";")) << r.cpp;
}

TEST(MethodGroupTranslation, LocalAndFieldHideTheMethod)
{
    const auto r = run(kFoo + "    void run() { int Hit = 1; take_int(Hit); }\n}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_FALSE(has(r, "m:this.Hit")) << r.cpp;
}

TEST(MethodGroupTranslation, GroupOutsideACallArgumentIsAnError)
{
    const std::string msg = "'Hit' is a method group. A method group is accepted only as a call argument";
    expect_error(kFoo + "    void run() { var f = Hit; }\n}\n", msg);
    expect_error(kFoo + "    void run() { var f = this.Hit; }\n}\n", msg);
    expect_error(kFoo + "    std.function<void(int)> f;\n    void run() { f = Hit; }\n}\n", msg);
    expect_error(kFoo + "    void run($Foo o) { if (o != null) { var f = o.Hit; } }\n}\n", msg);
    expect_error(kFoo + "    void run() { var f = Foo.Hit; }\n}\n", msg);
}

TEST(MethodGroupTranslation, OverloadedMethodIsAmbiguous)
{
    expect_error(kFoo + "    void Hit(float x) { }\n    void run() { take(Hit); }\n}\n",
        "ambiguous method group 'Hit': the method has overloads. Write a lambda");
}

// A field as the object is an error. The null check of the field reports it first.
TEST(MethodGroupTranslation, ObjectOfABoundGroupIsThisOrALocal)
{
    const auto r = run(kFoo + "    $Foo other;\n    void run() { if (other != null) { take(other.Hit); } }\n}\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(r.diagnostics.find("needs 'this' or a local as its object") != std::string::npos
        || r.diagnostics.find("copy it to a local") != std::string::npos)
        << r.diagnostics;
}

TEST(MethodGroupTranslation, UnboundGroupNeedsAMethodOfTheClass)
{
    expect_error("class Bar : Widget {\n    void Go(int x) { }\n}\n" + kFoo
            + "    void run(ref Sig<int> s) { s.Subscribe(this, Bar.Go); }\n}\n",
        "the unbound method group 'Bar.Go' needs a method of the class 'Foo'");
}

// A static method and a free function are not method groups.
TEST(MethodGroupTranslation, StaticMethodAndFreeFunctionAreNotGroups)
{
    expect_error(kFoo + "    static void S(int x) { }\n    void run() { take(S); }\n}\n",
        "'S' is a function; call it, or write a lambda");
    expect_error(kFoo + "    static void S(int x) { }\n    void run() { take(Foo.S); }\n}\n",
        "'S' is a function; call it, or write a lambda");
    expect_error("int Free(int x) { return x; }\n" + kFoo + "    void run() { take(Free); }\n}\n",
        "'Free' is a function; call it, or write a lambda");
}
