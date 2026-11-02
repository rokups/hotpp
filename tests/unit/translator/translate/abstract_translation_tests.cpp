// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests for hotc::translate with abstract script classes and methods: the checks, the trap
// members, the native pure virtual methods and the class record.
#include "hot/hotc_lib.h"
#include "support/translator_test_support.h"

#include <gtest/gtest.h>

#include <string>

using namespace translator_test;

namespace
{

const char* kAbstractHeader =
    "class Widget { public: virtual ~Widget() {} virtual void Update(float dt) {} };\n"
    "class IPing { public: virtual ~IPing() {} virtual int Ping(int n) = 0; };\n"
    "class IStrict { public: virtual ~IStrict() {} virtual int Seal(int x) noexcept = 0; };\n"
    "class IMix { public: virtual ~IMix() {} virtual int F(int a) = 0; virtual int F(int a, int b) { return a + b; }\n"
    "    virtual int G(int a) = 0; virtual int G(const char* s) { return 0; } };\n";

hotc::TranslateResult run(const std::string& classes, bool aot = false, bool optimized = false)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kAbstractHeader);
    hotc::TranslateOptions o = temp_options(dir, "using \"w.h\";\n" + classes);
    o.aot = aot;
    o.aot_optimized = optimized;
    return hotc::translate(o);
}

bool has(const std::string& text, const std::string& part)
{
    return text.find(part) != std::string::npos;
}

const char* kShape =
    "abstract class Shape : Widget {\n    abstract int Area();\n    int Twice() { return Area() * 2; }\n}\n";

} // namespace

TEST(AbstractTranslation, AbstractMethodGetsASlotAndATrapBody)
{
    const hotc::TranslateResult r =
        run(std::string(kShape) + "class Sq : Shape {\n    override int Area() { return 9; }\n}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "hot::abstract_called(\"Shape\", \"Area\");")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "Shape__Area_virt")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "ShapeState_g1::Area")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "Shape_record.is_abstract = true;")) << r.cpp;
    EXPECT_FALSE(has(r.cpp, "Sq_record.is_abstract")) << r.cpp;
}

TEST(AbstractTranslation, AbstractClassHasNoDefaultFactory)
{
    // The constructors give the classes a "new_default" function.
    const hotc::TranslateResult r = run(
        "abstract class Shape : Widget {\n    int n = 0;\n    Shape() { this.n = 1; }\n    abstract int Area();\n}\n"
        "class Sq : Shape {\n    override int Area() { return 9; }\n}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "Sq__new_default")) << r.cpp;
    EXPECT_FALSE(has(r.cpp, "&Shape__new_default")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "&Sq__new_default")) << r.cpp;
}

TEST(AbstractTranslation, OptimizedAotMemberTraps)
{
    const hotc::TranslateResult r =
        run(std::string(kShape) + "class Sq : Shape {\n    override int Area() { return 9; }\n}\n", true, true);
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "Shape_Shell::__b_Area(")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "hot::abstract_called(\"Shape\", \"Area\");")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "Shape_record.is_abstract = true;")) << r.cpp;
}

TEST(AbstractTranslation, ChildThatImplementsIsValid)
{
    // An override of a parent that implements the method is valid.
    const hotc::TranslateResult r = run(std::string(kShape)
        + "class Sq : Shape {\n    override int Area() { return 9; }\n}\nclass Sq2 : Sq { }\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

TEST(AbstractTranslation, AbstractChildOfAbstractNeedsNoImplementation)
{
    const hotc::TranslateResult r = run(std::string(kShape) + "abstract class Round : Shape { }\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

TEST(AbstractTranslation, ConcreteClassWithoutImplementationIsAnError)
{
    const hotc::TranslateResult r = run(std::string(kShape) + "class Sq : Shape { }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "class 'Sq' does not implement the abstract method 'Area' of 'Shape'"))
        << r.diagnostics;
}

TEST(AbstractTranslation, GrandchildOfAnImplementedMethodIsNotAnError)
{
    // Foo implements the method. Bar inherits it and must not report an error.
    const hotc::TranslateResult r = run(std::string(kShape)
        + "class Foo : Shape {\n    override int Area() { return 1; }\n}\nclass Bar : Foo { }\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

TEST(AbstractTranslation, NewOfAnAbstractClassIsAnError)
{
    const hotc::TranslateResult r = run(std::string(kShape)
        + "class Use : Widget {\n    Shape s = null;\n    void Make() { this.s = new Shape(); }\n}\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "cannot create an instance of the abstract class 'Shape'")) << r.diagnostics;
}

TEST(AbstractTranslation, BaseCallOfAnAbstractScriptMethodIsAnError)
{
    const hotc::TranslateResult r = run(std::string(kShape)
        + "class Sq : Shape {\n    override int Area() { return base.Area() + 1; }\n}\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "cannot call the abstract method 'Area' through 'base'")) << r.diagnostics;
}

TEST(AbstractTranslation, BaseCallOfAnImplementedMethodIsValid)
{
    const hotc::TranslateResult r = run(std::string(kShape)
        + "class Sq : Shape {\n    override int Area() { return 9; }\n}\n"
          "class Cube : Sq {\n    override int Area() { return base.Area() * 3; }\n}\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

TEST(AbstractTranslation, PureNativeMethodNeedsAnImplementation)
{
    const hotc::TranslateResult r = run("class Foo : Widget, IPing { }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "class Foo does not override the pure virtual method 'Ping(int)' of IPing"))
        << r.diagnostics;
}

TEST(AbstractTranslation, ChildImplementsAPureNativeMethodOfAnAbstractParent)
{
    const std::string src = "abstract class Base : Widget, IPing { }\n"
                            "class Impl : Base {\n    override int Ping(int n) { return n; }\n}\n"
                            "class Impl2 : Impl { }\n";
    const hotc::TranslateResult r = run(src);
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "Base_record.is_abstract = true;")) << r.cpp;
    EXPECT_FALSE(has(r.cpp, "Impl_record.is_abstract")) << r.cpp;
    EXPECT_FALSE(has(r.cpp, "&Base__new_default")) << r.cpp;
}

TEST(AbstractTranslation, ChildOfAnAbstractParentWithoutTheNativeImplementationIsAnError)
{
    const hotc::TranslateResult r = run("abstract class Base : Widget, IPing { }\nclass Impl : Base { }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "class Impl does not override the pure virtual method 'Ping(int)' of IPing"))
        << r.diagnostics;
}

TEST(AbstractTranslation, PureNativeBaseMethodIsATrapAndNeverCallsTheBase)
{
    for (const bool optimized : {false, true})
    {
        const hotc::TranslateResult r = run("abstract class Base : Widget, IPing { }\n", optimized, optimized);
        ASSERT_TRUE(r.ok) << r.diagnostics;
        EXPECT_FALSE(has(r.cpp, "IPing::Ping(")) << r.cpp;
        EXPECT_TRUE(has(r.cpp, "hot::abstract_called(\"Base\", \"Ping\");")) << r.cpp;
        // The optimized shell overrides the pure method itself, or the shell stays abstract in C++.
        EXPECT_TRUE(has(r.cpp, "int Ping(int a0) override")) << r.cpp;
    }
}

TEST(AbstractTranslation, BaseCallOfAPureNativeMethodIsAnError)
{
    const hotc::TranslateResult r = run("abstract class Base : Widget, IPing { }\n"
                                        "class Impl : Base {\n    override int Ping(int n) { return base.Ping(n); }\n}\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "cannot call the abstract method 'Ping' through 'base'")) << r.diagnostics;
}

TEST(AbstractTranslation, BaseCallOfAPureNativeMethodImplementedByTheParentIsValid)
{
    const hotc::TranslateResult r = run(
        "abstract class Base : Widget, IPing { }\n"
        "class Impl : Base {\n    override int Ping(int n) { return n; }\n}\n"
        "class Deep : Impl {\n    override int Ping(int n) { return base.Ping(n) + 1; }\n}\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

TEST(AbstractTranslation, BaseCallOfAPureNativeMethodInTheRootIsAnError)
{
    const hotc::TranslateResult r =
        run("class Impl : Widget, IPing {\n    override int Ping(int n) { return base.Ping(n); }\n}\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "cannot call the abstract method 'Ping' through 'base'")) << r.diagnostics;
}

TEST(AbstractTranslation, NoexceptPureNativeTrapIsNotNoexceptInTheBaseFunction)
{
    for (const bool optimized : {false, true})
    {
        const hotc::TranslateResult r = run("abstract class Base : Widget, IStrict { }\n", optimized, optimized);
        ASSERT_TRUE(r.ok) << r.diagnostics;
        // The trap throws, so the base function has no noexcept. The shell override reports the error.
        EXPECT_FALSE(has(r.cpp, "__hot_base_Seal(int a0) noexcept")) << r.cpp;
        if (optimized)
            EXPECT_TRUE(has(r.cpp, "hot::call_direct<int, true>(this, [&]() -> int { hot::abstract_called(\"Base\", \"Seal\"); })"))
                << r.cpp;
        else
            EXPECT_TRUE(has(r.cpp, "hot::call<int(Widget*, int), true>")) << r.cpp;
    }
}

TEST(AbstractTranslation, BaseCallWithAnOverloadThatHasABodyIsNotAnError)
{
    // F(int) is pure. F(int, int) has a body. Two arguments select the body.
    const std::string src = "abstract class Base : Widget, IMix { }\n"
                            "class Impl : Base {\n    override int F(int a) { return a; }\n"
                            "    override int G(int a) { return a; }\n}\n"
                            "abstract class Open : Base {\n    int Use() { return base.F(1, 2); }\n}\n";
    EXPECT_TRUE(run(src).ok) << run(src).diagnostics;
    // The same count: the pass cannot tell the overloads apart, so it reports nothing.
    const std::string same = "abstract class Open : Widget, IMix {\n    int Use() { return base.G(\"x\"); }\n}\n";
    const hotc::TranslateResult r = run(same);
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

TEST(AbstractTranslation, BaseCallWithTheCountOfThePureOverloadIsAnError)
{
    const std::string src = "abstract class Open : Widget, IMix {\n    int Use() { return base.F(1); }\n}\n";
    const hotc::TranslateResult r = run(src);
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "cannot call the abstract method 'F' through 'base'")) << r.diagnostics;
}

TEST(AbstractTranslation, AbstractAsyncMethod)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kAbstractHeader);
    hotc::TranslateOptions o = temp_options(dir,
        "using \"w.h\";\nabstract class Job : Widget {\n    abstract async int Run();\n}\n"
        "class Work : Job {\n    override async int Run() { return 1; }\n}\n");
    o.std_version = "c++20";
    const hotc::TranslateResult r = hotc::translate(o);
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "hot::abstract_called(\"Job\", \"Run\");")) << r.cpp;
    // The abstract member has no coroutine: an empty coroutine body would not compile.
    EXPECT_FALSE(has(r.cpp, "Job__Run_co")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "Work__Run_co")) << r.cpp;
}

TEST(AbstractTranslation, AbstractMethodWithValueTypes)
{
    const hotc::TranslateResult r = run(
        "class P {\n    int x = 0;\n}\n"
        "abstract class Maker : Widget {\n    [value] abstract P Make([value] P seed);\n}\n"
        "class Real : Maker {\n    [value] override P Make([value] P seed) { return seed; }\n}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "hot::abstract_called(\"Maker\", \"Make\");")) << r.cpp;
}

TEST(AbstractTranslation, ReAbstractingAnOverrideIsAParseError)
{
    const hotc::TranslateResult r = run(std::string(kShape)
        + "abstract class Sq : Shape {\n    abstract override int Area();\n}\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "'abstract' and 'override' cannot combine")) << r.diagnostics;
}
