// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests for hotc::translate with script interfaces (design 41b, step B3): the base list, the
// conformance check and the errors for the use of an interface as a type.
#include "hot/hotc_lib.h"
#include "support/translator_test_support.h"

#include <gtest/gtest.h>

#include <string>

using namespace translator_test;

namespace
{

const char* kHeader =
    "class Widget { public: virtual ~Widget() {} virtual void Update(float dt) {} };\n"
    "class IPing { public: virtual ~IPing() {} virtual int Ping(int n) = 0; };\n";

hotc::TranslateResult run(const std::string& script)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kHeader);
    return hotc::translate(temp_options(dir, "using \"w.h\";\n" + script));
}

bool has(const std::string& text, const std::string& part)
{
    return text.find(part) != std::string::npos;
}

const char* kShape = "interface IShape {\n    int Area();\n    void Scale(int k);\n}\n";
const char* kNamed = "interface INamed {\n    string Name();\n}\n";

} // namespace

TEST(ScriptInterfaceTranslation, OwnMethodsConform)
{
    const hotc::TranslateResult r = run(std::string(kShape)
        + "class Sq : Widget, IShape {\n    int Area() { return 4; }\n    void Scale(int k) { }\n}\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

TEST(ScriptInterfaceTranslation, InheritedMethodsConform)
{
    const hotc::TranslateResult r = run(std::string(kShape)
        + "class Base : Widget {\n    int Area() { return 4; }\n    void Scale(int k) { }\n}\n"
          "class Sq : Base, IShape {\n}\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

TEST(ScriptInterfaceTranslation, ParentInterfaceNeedsMethodsOfTheChild)
{
    // The child inherits the interface of the parent. The parent is abstract, so it passes.
    const hotc::TranslateResult ok = run(std::string(kShape)
        + "abstract class Base : Widget, IShape {\n}\n"
          "class Sq : Base {\n    int Area() { return 4; }\n    void Scale(int k) { }\n}\n");
    EXPECT_TRUE(ok.ok) << ok.diagnostics;
    const hotc::TranslateResult bad = run(std::string(kShape)
        + "abstract class Base : Widget, IShape {\n}\n"
          "class Sq : Base {\n    int Area() { return 4; }\n}\n");
    EXPECT_FALSE(bad.ok);
    EXPECT_TRUE(has(bad.diagnostics, "class 'Sq' does not implement 'IShape.Scale'")) << bad.diagnostics;
}

TEST(ScriptInterfaceTranslation, AbstractMethodConforms)
{
    const hotc::TranslateResult r = run(std::string(kShape)
        + "abstract class Base : Widget, IShape {\n    abstract int Area();\n    abstract void Scale(int k);\n}\n"
          "class Sq : Base {\n    override int Area() { return 4; }\n    override void Scale(int k) { }\n}\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

TEST(ScriptInterfaceTranslation, AbstractClassNeedsNoMethods)
{
    const hotc::TranslateResult r = run(std::string(kShape) + "abstract class Base : Widget, IShape {\n}\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

TEST(ScriptInterfaceTranslation, ClassWithTwoInterfaces)
{
    const hotc::TranslateResult r = run(std::string(kShape) + kNamed
        + "class Sq : Widget, IShape, INamed {\n    int Area() { return 4; }\n    void Scale(int k) { }\n"
          "    string Name() { return \"sq\"; }\n}\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
    const hotc::TranslateResult bad = run(std::string(kShape) + kNamed
        + "class Sq : Widget, IShape, INamed {\n    int Area() { return 4; }\n    void Scale(int k) { }\n}\n");
    EXPECT_FALSE(bad.ok);
    EXPECT_TRUE(has(bad.diagnostics, "class 'Sq' does not implement 'INamed.Name'")) << bad.diagnostics;
}

TEST(ScriptInterfaceTranslation, NativeInterfaceBaseStillWorks)
{
    const hotc::TranslateResult r = run(std::string(kShape)
        + "class Sq : Widget, IPing, IShape {\n    override int Ping(int n) { return n; }\n"
          "    int Area() { return 4; }\n    void Scale(int k) { }\n}\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
    const hotc::TranslateResult native_only = run("class Sq : Widget, IPing {\n    override int Ping(int n) { return n; }\n}\n");
    EXPECT_TRUE(native_only.ok) << native_only.diagnostics;
}

TEST(ScriptInterfaceTranslation, MissingMethodIsAnError)
{
    const hotc::TranslateResult r = run(std::string(kShape) + "class Sq : Widget, IShape {\n    int Area() { return 4; }\n}\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "class 'Sq' does not implement 'IShape.Scale'")) << r.diagnostics;
}

TEST(ScriptInterfaceTranslation, OtherParameterTypeIsAnError)
{
    const hotc::TranslateResult r = run(std::string(kShape)
        + "class Sq : Widget, IShape {\n    int Area() { return 4; }\n    void Scale(float k) { }\n}\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "class 'Sq' does not implement 'IShape.Scale(int)': the method 'Scale' has another signature"))
        << r.diagnostics;
}

TEST(ScriptInterfaceTranslation, OtherParameterFormIsAnError)
{
    const hotc::TranslateResult r = run(std::string(kShape)
        + "class Sq : Widget, IShape {\n    int Area() { return 4; }\n    void Scale(int& k) { }\n}\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "another signature")) << r.diagnostics;
}

TEST(ScriptInterfaceTranslation, OtherReturnTypeIsAnError)
{
    const hotc::TranslateResult r = run(std::string(kShape)
        + "class Sq : Widget, IShape {\n    float Area() { return 4; }\n    void Scale(int k) { }\n}\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "class 'Sq' does not implement 'IShape.Area()': the method 'Area' has another signature"))
        << r.diagnostics;
}

TEST(ScriptInterfaceTranslation, PrivateMethodDoesNotConform)
{
    const hotc::TranslateResult r = run(std::string(kShape)
        + "class Sq : Widget, IShape {\n    int Area() { return 4; }\n    private void Scale(int k) { }\n}\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "class 'Sq' does not implement 'IShape.Scale'")) << r.diagnostics;
}

TEST(ScriptInterfaceTranslation, ClassAndInterfaceWithOneNameIsAnError)
{
    const hotc::TranslateResult r = run("interface Thing {\n    int F();\n}\nclass Thing : Widget {\n}\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "the name 'Thing' is a class and an interface")) << r.diagnostics;
}

TEST(ScriptInterfaceTranslation, InterfaceAsFirstNameMeansNoParent)
{
    const hotc::TranslateResult r = run(std::string(kShape)
        + "class Sq : IShape {\n    int Area() { return 4; }\n    void Scale(int k) { }\n}\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
    const hotc::TranslateResult bad = run(std::string(kShape)
        + "class Sq : IShape {\n    int Area() { return 4; }\n}\n");
    EXPECT_FALSE(bad.ok);
    EXPECT_TRUE(has(bad.diagnostics, "class 'Sq' does not implement 'IShape.Scale'")) << bad.diagnostics;
}

TEST(ScriptInterfaceTranslation, TwoInterfacesWithoutParent)
{
    const hotc::TranslateResult r = run(std::string(kShape) + kNamed
        + "class Sq : IShape, INamed {\n    int Area() { return 4; }\n    void Scale(int k) { }\n"
          "    string Name() { return \"sq\"; }\n}\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

TEST(ScriptInterfaceTranslation, SameCodeAsAClassWithoutBaseList)
{
    const hotc::TranslateResult with = run(std::string(kShape)
        + "class Sq : IShape {\n    int Area() { return 4; }\n    void Scale(int k) { }\n}\n");
    const hotc::TranslateResult without = run(std::string(kShape)
        + "class Sq {\n    int Area() { return 4; }\n    void Scale(int k) { }\n}\n");
    ASSERT_TRUE(with.ok) << with.diagnostics;
    ASSERT_TRUE(without.ok) << without.diagnostics;
    EXPECT_EQ(with.cpp, without.cpp);
}

TEST(ScriptInterfaceTranslation, ParentAfterInterfaceIsAnError)
{
    const hotc::TranslateResult r = run(std::string(kShape)
        + "class Base : Widget {\n}\nclass C : IShape, Base {\n}\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "the interface 'IShape' cannot be the base class of 'C'")) << r.diagnostics;
    EXPECT_TRUE(has(r.diagnostics, "list the base class 'Base' first")) << r.diagnostics;
}

TEST(ScriptInterfaceTranslation, StructCannotImplementAnInterface)
{
    const hotc::TranslateResult r = run(std::string(kShape) + "struct P : IShape {\n}\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "a struct cannot implement the interface 'IShape'")) << r.diagnostics;
    const hotc::TranslateResult r2 = run(std::string(kShape) + "struct P : Widget, IShape {\n}\n");
    EXPECT_FALSE(r2.ok);
    EXPECT_TRUE(has(r2.diagnostics, "a struct cannot implement the interface 'IShape'")) << r2.diagnostics;
}

TEST(ScriptInterfaceTranslation, SameInterfaceTwiceIsAnError)
{
    const hotc::TranslateResult r = run(std::string(kShape) + "class Sq : Widget, IShape, IShape {\n}\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "'IShape' is listed twice")) << r.diagnostics;
}

TEST(ScriptInterfaceTranslation, UnknownBaseKeepsItsError)
{
    const hotc::TranslateResult r = run(std::string(kShape) + "class Sq : Widget, INothing {\n}\n");
    EXPECT_FALSE(r.ok);
    EXPECT_FALSE(has(r.diagnostics, "listed twice")) << r.diagnostics;
    EXPECT_TRUE(has(r.diagnostics, "INothing")) << r.diagnostics;
}

TEST(ScriptInterfaceTranslation, NewOfAnInterfaceIsAnError)
{
    const hotc::TranslateResult r = run(std::string(kShape)
        + "class Sq : Widget {\n    void Go() { var x = new IShape(); }\n}\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "cannot create an instance of the interface 'IShape'")) << r.diagnostics;
}

TEST(ScriptInterfaceTranslation, WeakInterfaceIsAnError)
{
    const hotc::TranslateResult field = run(std::string(kShape) + "class Sq : Widget {\n    [weak] IShape s;\n}\n");
    EXPECT_FALSE(field.ok);
    EXPECT_TRUE(has(field.diagnostics, "[weak] cannot be used with the interface 'IShape'")) << field.diagnostics;
    const hotc::TranslateResult local = run(std::string(kShape)
        + "class Sq : Widget {\n    void Go() { [weak] IShape s = null; }\n}\n");
    EXPECT_FALSE(local.ok);
    EXPECT_TRUE(has(local.diagnostics, "[weak] cannot be used with the interface 'IShape'")) << local.diagnostics;
}

TEST(ScriptInterfaceTranslation, ValueInterfaceIsAnError)
{
    const hotc::TranslateResult field = run(std::string(kShape) + "class Sq : Widget {\n    [value] IShape s;\n}\n");
    EXPECT_FALSE(field.ok);
    EXPECT_TRUE(has(field.diagnostics, "[value] cannot be used with the interface 'IShape'")) << field.diagnostics;
    const hotc::TranslateResult local = run(std::string(kShape)
        + "class Sq : Widget {\n    void Go() { [value] IShape s; }\n}\n");
    EXPECT_FALSE(local.ok);
    EXPECT_TRUE(has(local.diagnostics, "[value] cannot be used with the interface 'IShape'")) << local.diagnostics;
    EXPECT_FALSE(has(local.diagnostics, "cannot create an instance")) << local.diagnostics;
}

TEST(ScriptInterfaceTranslation, AliasInInterfaceAndClassConforms)
{
    const hotc::TranslateResult r = run(
        "using Count = int;\ninterface ICount {\n    Count Get(Count k);\n}\n"
        "class C : Widget, ICount {\n    int Get(int k) { return k; }\n}\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
    const hotc::TranslateResult r2 = run(
        "interface ICount {\n    int Get(int k);\n}\n"
        "class C : Widget, ICount {\n    using Count = int;\n    Count Get(Count k) { return k; }\n}\n");
    EXPECT_TRUE(r2.ok) << r2.diagnostics;
}

TEST(ScriptInterfaceTranslation, OverloadWithOtherSignatureDoesNotHideTheMatch)
{
    const hotc::TranslateResult r = run(std::string(kShape)
        + "class Sq : Widget, IShape {\n    int Area() { return 4; }\n    void Scale(int k) { }\n"
          "    void Scale(float k) { }\n}\n");
    // A script class has no overloads, so the translator rejects the second declaration. The
    // error is not a conformance error.
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "function 'Scale' is declared twice")) << r.diagnostics;
}

TEST(ScriptInterfaceTranslation, GrandparentImplementsTheMethod)
{
    const hotc::TranslateResult r = run(std::string(kShape)
        + "class A : Widget {\n    int Area() { return 4; }\n    void Scale(int k) { }\n}\n"
          "class B : A {\n}\nclass C : B, IShape {\n}\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

TEST(ScriptInterfaceTranslation, InterfaceOnParentAndChildIsNoError)
{
    const hotc::TranslateResult r = run(std::string(kShape)
        + "class A : Widget, IShape {\n    int Area() { return 4; }\n    void Scale(int k) { }\n}\n"
          "class B : A, IShape {\n}\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

TEST(ScriptInterfaceTranslation, StaticMethodIsRejectedBeforeConformance)
{
    // A script class has no static method, so a static method cannot satisfy an interface.
    const hotc::TranslateResult r = run(std::string(kShape)
        + "class Sq : Widget, IShape {\n    static int Area() { return 4; }\n    void Scale(int k) { }\n}\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "static members are not supported")) << r.diagnostics;
}

TEST(ScriptInterfaceTranslation, AsyncMethodDoesNotConform)
{
    const hotc::TranslateResult r = run(std::string(kShape)
        + "class Sq : Widget, IShape {\n    async int Area() { return 4; }\n    void Scale(int k) { }\n}\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "class 'Sq' does not implement 'IShape.Area': the method 'Area' is async"))
        << r.diagnostics;
}
