// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests for hotc::translate with script interfaces (design 41b, step B3): the base list, the
// conformance check and the errors for the use of an interface as a type.
#include "hot/hotc_lib.h"
#include "support/translator_test_support.h"

#include <gtest/gtest.h>

#include <sstream>
#include <string>
#include <vector>

using namespace translator_test;

namespace
{

const char* kHeader =
    "class Widget { public: virtual ~Widget() {} virtual void Update(float dt) {} };\n"
    "class IPing { public: virtual ~IPing() {} virtual int Ping(int n) = 0; };\n"
    "#include <string>\n"
    "class Nat : public Widget { public: virtual int Foo(int) { return 0; } virtual int Foo(float) { return 1; }\n"
    "    virtual int Area() { return 0; } virtual int Take(std::string) { return 0; } };\n";

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
    // The class with the list differs in the interface rows only (B5). The line markers move.
    auto strip = [](const std::string& text)
    {
        std::string out, line;
        std::istringstream in(text);
        while (std::getline(in, line))
            if (!line.empty() && line.rfind("#line", 0) != 0 && line.find(".ifaces =") == std::string::npos
                && line.find("iface_rows") == std::string::npos && line.find("__ifaces[]") == std::string::npos
                && line.find("{\"IShape\", ") == std::string::npos
                && line.find("{nullptr, nullptr, nullptr, 0}};") == std::string::npos)
                out += line + "\n";
        return out;
    };
    EXPECT_EQ(strip(with.cpp), strip(without.cpp));
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

// ---- Types and lowering (design 41b, step B4)
namespace
{

const char* kSq =
    "class Sq : Widget, IShape {\n    int Area() { return 4; }\n    void Scale(int k) { }\n}\n";

// A script with the interface IShape, the class Sq that implements it and the class "User" with "body".
hotc::TranslateResult run_user(const std::string& body)
{
    return run(std::string(kShape) + kSq + "class User : Widget {\n" + body + "}\n");
}

} // namespace

// The storage of an interface type is hot::Iface<I__iface> (D4).
TEST(ScriptInterfaceTranslation, StorageOfFieldsParametersAndReturns)
{
    const hotc::TranslateResult r = run_user(
        "    IShape f;\n    IShape Make(IShape p, const IShape& q) { return p; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "    hot::Iface<IShape__iface> f;")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "hot::Iface<IShape__iface> Make(const hot::Iface<IShape__iface>& p"))
        << r.cpp;
    EXPECT_TRUE(has(r.cpp, "User__Make(User_Shell* __self, hot::Iface<IShape__iface> p")) << r.cpp;
}

TEST(ScriptInterfaceTranslation, CollectionOfInterfaces)
{
    const hotc::TranslateResult r = run_user(
        "    List<IShape> xs;\n    std.vector<IShape> ys;\n    std.map<string, IShape> m;\n"
        "    void Go(IShape s) { IShape last = ys[0]; ys.push_back(s); }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "List<hot::Iface<IShape__iface>> xs;")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "std::vector<hot::Iface<IShape__iface>> ys;")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "std::map<string, hot::Iface<IShape__iface>> m;")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "hot::Iface<IShape__iface> last = hot::index(ys, 0);")) << r.cpp;
}

TEST(ScriptInterfaceTranslation, SharedPtrOfAnInterfaceKeepsItsArgument)
{
    // A smart pointer template takes the argument as it is.
    const hotc::TranslateResult r = run_user("    std.shared_ptr<IShape> p;\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "std::shared_ptr<IShape> p;")) << r.cpp;
}

TEST(ScriptInterfaceTranslation, LocalAndNull)
{
    const hotc::TranslateResult r = run_user(
        "    void Go() { IShape a; IShape b = null; var c = b; b = null; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "hot::Iface<IShape__iface> a = hot::Iface<IShape__iface>();")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "hot::Iface<IShape__iface> b = hot::Iface<IShape__iface>();")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "auto c = b;")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "b = hot::Iface<IShape__iface>();")) << r.cpp;
}

TEST(ScriptInterfaceTranslation, ClassHandleConvertsInLocalAndAssignment)
{
    const hotc::TranslateResult r = run_user(
        "    void Go() { Sq q = new Sq(); IShape a = q; IShape b = new Sq(); b = q; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "hot::Iface<IShape__iface> a = hot::to_iface<IShape__iface>(q);")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "hot::Iface<IShape__iface> b = hot::to_iface<IShape__iface>(std::shared_ptr<Sq>(Sq__new_shared()));"))
        << r.cpp;
    EXPECT_TRUE(has(r.cpp, "b = hot::to_iface<IShape__iface>(q);")) << r.cpp;
}

TEST(ScriptInterfaceTranslation, InterfaceValueNeedsNoConversion)
{
    const hotc::TranslateResult r = run_user("    void Go(IShape p) { IShape a = p; a = p; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "hot::Iface<IShape__iface> a = p;")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "a = p;")) << r.cpp;
    EXPECT_FALSE(has(r.cpp, "to_iface")) << r.cpp;
}

TEST(ScriptInterfaceTranslation, ClassHandleConvertsInArgumentAndReturn)
{
    const hotc::TranslateResult r = run_user(
        "    void Use(IShape s) { }\n    IShape Make() { return new Sq(); }\n    IShape Pass(Sq q) { Use(q); return q; }\n"
        "    IShape Nothing() { return null; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "User__Use(__self, hot::to_iface<IShape__iface>(q));")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "return hot::to_iface<IShape__iface>(std::shared_ptr<Sq>(Sq__new_shared()));")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "return hot::to_iface<IShape__iface>(q);")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "return hot::Iface<IShape__iface>();")) << r.cpp;
}

TEST(ScriptInterfaceTranslation, NullArgument)
{
    const hotc::TranslateResult r = run_user("    void Use(IShape s) { }\n    void Go() { Use(null); }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "User__Use(__self, hot::Iface<IShape__iface>());")) << r.cpp;
}

TEST(ScriptInterfaceTranslation, FieldInitializerAndDefaultArgument)
{
    const hotc::TranslateResult r = run_user(
        "    IShape f = new Sq();\n    IShape g = null;\n    void Use(IShape s = null) { }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "hot::to_iface<IShape__iface>(std::shared_ptr<Sq>(Sq__new_shared()))")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "g = hot::Iface<IShape__iface>()") || has(r.cpp, "T(hot::Iface<IShape__iface>())")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "const hot::Iface<IShape__iface>& s = hot::Iface<IShape__iface>()")) << r.cpp;
}

TEST(ScriptInterfaceTranslation, ThisConvertsInAClassThatListsTheInterface)
{
    const hotc::TranslateResult r = run(std::string(kShape)
        + "class Reg : Widget {\n    void Add(IShape s) { }\n}\n"
          "class Sq : Widget, IShape {\n    int Area() { return 4; }\n    void Scale(int k) { }\n"
          "    void Go(Reg r) { r.Add(this); }\n    IShape Me() { return this; }\n}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "hot::to_iface<IShape__iface>(hot::handle_from_this<")) << r.cpp;
}

TEST(ScriptInterfaceTranslation, ThisInAParentThatListsTheInterface)
{
    const hotc::TranslateResult r = run(std::string(kShape)
        + "class Base : Widget, IShape {\n    int Area() { return 4; }\n    void Scale(int k) { }\n}\n"
          "class Sq : Base {\n    IShape Me() { return this; }\n}\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

TEST(ScriptInterfaceTranslation, ClassWithoutTheInterfaceIsAnError)
{
    const hotc::TranslateResult r = run(std::string(kShape)
        + "class Plain : Widget {\n    IShape Me() { return this; }\n    IShape Make() { return new Plain(); }\n}\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "the class 'Plain' does not implement the interface 'IShape'")) << r.diagnostics;
}

TEST(ScriptInterfaceTranslation, ParentClassHandleConvertsWhenTheParentListsTheInterface)
{
    const hotc::TranslateResult r = run(std::string(kShape)
        + "class Base : Widget, IShape {\n    int Area() { return 4; }\n    void Scale(int k) { }\n}\n"
          "class Sq : Base {\n}\nclass User : Widget {\n    void Go(Sq q) { IShape a = q; }\n}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "hot::Iface<IShape__iface> a = hot::to_iface<IShape__iface>(q);")) << r.cpp;
}

TEST(ScriptInterfaceTranslation, OtherInterfaceDoesNotConvert)
{
    const hotc::TranslateResult r = run(std::string(kShape) + kNamed
        + "class User : Widget {\n    void Go(IShape a) { INamed n = a; }\n}\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "the interface 'IShape' does not convert to the interface 'INamed'")) << r.diagnostics;
}

TEST(ScriptInterfaceTranslation, PushBackConverts)
{
    const hotc::TranslateResult r = run_user(
        "    std.vector<IShape> xs;\n    void Go(Sq q) { xs.push_back(q); xs.push_back(null); }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "hot::deref(__st->xs).push_back(hot::to_iface<IShape__iface>(q));")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "hot::deref(__st->xs).push_back(hot::Iface<IShape__iface>());")) << r.cpp;
}

TEST(ScriptInterfaceTranslation, CallLowersToTheMethodHelper)
{
    const hotc::TranslateResult r = run_user(
        "    IShape f;\n    int Go(IShape s) { s.Scale(2); this.f.Scale(3); return s.Area() + f.Area(); }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "IShape__Scale(s, 2);")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "IShape__Scale(__st->f, 3);")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "return IShape__Area(s) + IShape__Area(__st->f);")) << r.cpp;
}

TEST(ScriptInterfaceTranslation, NullInterfaceCallUsesTheSameForm)
{
    // The helper throws for a null handle, so the call has no extra check.
    const hotc::TranslateResult r = run_user("    int Go() { IShape s = null; return s.Area(); }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "return IShape__Area(s);")) << r.cpp;
    EXPECT_FALSE(has(r.cpp, "IShape__Area(hot::deref")) << r.cpp;
}

TEST(ScriptInterfaceTranslation, SafeCall)
{
    const hotc::TranslateResult r = run_user("    void Go(IShape s) { s?.Scale(2); }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "if (s)")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "IShape__Scale(s, 2);")) << r.cpp;
}

TEST(ScriptInterfaceTranslation, CallConvertsClassHandleArgument)
{
    const hotc::TranslateResult r = run(std::string(kShape)
        + "interface IReg {\n    void Add(IShape s);\n}\n" + kSq
        + "class User : Widget {\n    void Go(IReg reg, Sq q) { reg.Add(q); reg.Add(null); }\n}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "IReg__Add(reg, hot::to_iface<IShape__iface>(q));")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "IReg__Add(reg, hot::Iface<IShape__iface>());")) << r.cpp;
}

TEST(ScriptInterfaceTranslation, ChainedCallOnAnInterfaceResult)
{
    const hotc::TranslateResult r = run(std::string(kShape)
        + "interface IReg {\n    IShape First();\n}\nclass User : Widget {\n    int Go(IReg reg) { return reg.First().Area(); }\n}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "return IShape__Area(IReg__First(reg));")) << r.cpp;
}

TEST(ScriptInterfaceTranslation, UnknownMethodIsAnError)
{
    const hotc::TranslateResult r = run_user("    void Go(IShape s) { s.Nothing(); }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "the interface 'IShape' has no method 'Nothing'")) << r.diagnostics;
}

TEST(ScriptInterfaceTranslation, WrongArgumentCountIsAnError)
{
    const hotc::TranslateResult r = run_user("    void Go(IShape s) { s.Scale(); }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "the interface method 'IShape.Scale' takes 1 arguments, and the call has 0"))
        << r.diagnostics;
}

TEST(ScriptInterfaceTranslation, MemberOfAnInterfaceIsAnError)
{
    const hotc::TranslateResult r = run_user("    int Go(IShape s) { return s.size; }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "an interface has methods only")) << r.diagnostics;
}

TEST(ScriptInterfaceTranslation, IsInterface)
{
    const hotc::TranslateResult r = run_user("    bool Go(Widget w, Sq q) { return w is IShape || q is IShape; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "hot::is_iface(w, \"IShape\") || hot::is_iface(q, \"IShape\")")) << r.cpp;
}

TEST(ScriptInterfaceTranslation, AsInterface)
{
    const hotc::TranslateResult r = run_user("    IShape Go(Widget w) { IShape s = w as IShape; return w as IShape; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "hot::Iface<IShape__iface> s = hot::as_iface<IShape__iface>(w, \"IShape\");")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "return hot::as_iface<IShape__iface>(w, \"IShape\");")) << r.cpp;
}

TEST(ScriptInterfaceTranslation, IsInterfaceWithBinding)
{
    const hotc::TranslateResult r = run_user("    int Go(Widget w) { if (w is IShape g) return g.Area(); return 0; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "auto g = hot::as_iface<IShape__iface>(w, \"IShape\");")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "return IShape__Area(g);")) << r.cpp;
}

TEST(ScriptInterfaceTranslation, InterfaceValueIsAClass)
{
    const hotc::TranslateResult r = run_user("    bool Go(IShape s) { return s is Sq; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "return hot::is_script(s, \"Sq\");")) << r.cpp;
}

TEST(ScriptInterfaceTranslation, InterfaceValueAsClass)
{
    const hotc::TranslateResult r = run_user("    Sq Go(IShape s) { Sq q = s as Sq; return s as Sq; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "std::shared_ptr<Sq> q = hot::iface_as<std::shared_ptr<Sq>, Sq_Shell>(s, \"Sq\");")
        || has(r.cpp, "q = hot::iface_as<std::shared_ptr<Sq>, Sq_Shell>(s, \"Sq\");")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "return hot::iface_as<std::shared_ptr<Sq>, Sq_Shell>(s, \"Sq\");")) << r.cpp;
}

TEST(ScriptInterfaceTranslation, AsInterfaceKeepsTheTypeForACall)
{
    const hotc::TranslateResult r = run_user("    int Go(Widget w) { var s = w as IShape; return s.Area(); }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "auto s = hot::as_iface<IShape__iface>(w, \"IShape\");")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "return IShape__Area(s);")) << r.cpp;
}

TEST(ScriptInterfaceTranslation, EqualityComparesTheObject)
{
    const hotc::TranslateResult r = run_user(
        "    bool Go(IShape a, IShape b, Sq q) { return a == b || a != b || a == q || q != a; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "hot::iface_same(a, b)")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "!hot::iface_same(a, b)")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "hot::iface_same(a, q)")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "!hot::iface_same(q, a)")) << r.cpp;
}

TEST(ScriptInterfaceTranslation, EqualityOfThis)
{
    const hotc::TranslateResult r = run(std::string(kShape)
        + "class Me : Widget, IShape {\n    int Area() { return 1; }\n    void Scale(int k) { }\n"
          "    bool Same(IShape i) { return this == i; }\n    bool Other(IShape i) { return i != this; }\n}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "hot::iface_same(hot::handle_from_this<")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "!hot::iface_same(i, hot::handle_from_this<")) << r.cpp;
}

// The code of a script interface (B5): the tag, the declaration and the definition of the helper
// of each method, and the rows and the names in the record of a class.
TEST(ScriptInterfaceTranslation, EmitsTheTagTheHelpersAndTheRows)
{
    const hotc::TranslateResult r = run(std::string(kShape)
        + "class Base : Widget {\n    virtual int Area() { return 4; }\n    void Scale(int k) { }\n}\n"
          "class Sq : Base, IShape {\n    override int Area() { return 5; }\n}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "struct IShape__iface;")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "inline int32_t IShape__Area(const hot::Iface<IShape__iface>& h);")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "static std::atomic<uint32_t> site{0};")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "hot::call_iface<void(int32_t)>(h, site, \"IShape.Scale\", std::forward<int32_t>(a0))"))
        << r.cpp;
    EXPECT_TRUE(has(r.cpp, "{\"IShape\", \"Area\", typeid(int32_t()).name(), ")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "{\"IShape\", \"Scale\", typeid(void(int32_t)).name(), ")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "Sq__ifaces[] = {\"IShape\", nullptr};")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "Sq_record.iface_rows = Sq__iface_rows;")) << r.cpp;
    EXPECT_FALSE(has(r.cpp, "Base_record.iface_rows")) << r.cpp;
}

TEST(ScriptInterfaceTranslation, OptimizedAotRejectsInterfaces)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kHeader);
    hotc::TranslateOptions o = temp_options(dir, std::string("using \"w.h\";\n") + kShape);
    o.aot = true;
    o.aot_optimized = true;
    const hotc::TranslateResult r = hotc::translate(o);
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "script interfaces are not supported in the optimized AOT mode")) << r.diagnostics;
    o.aot_optimized = false;
    EXPECT_TRUE(hotc::translate(o).ok);
}

TEST(ScriptInterfaceTranslation, EqualityWithNull)
{
    const hotc::TranslateResult r = run_user(
        "    bool Go(IShape a) { if (a != null) return a == null; return null == a; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "!hot::iface_same(a, nullptr)")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "return hot::iface_same(a, nullptr);")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "return hot::iface_same(nullptr, a);")) << r.cpp;
}

TEST(ScriptInterfaceTranslation, ClassComparisonKeepsItsForm)
{
    const hotc::TranslateResult r = run_user("    bool Go(Sq a, Sq b) { return a == b && a != null; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_FALSE(has(r.cpp, "iface_same")) << r.cpp;
}

TEST(ScriptInterfaceTranslation, StructCannotConvert)
{
    const hotc::TranslateResult r = run(std::string(kShape)
        + "struct P {\n    int x;\n}\nclass User : Widget {\n    void Go(P p) { IShape a = p; }\n}\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "the struct 'P' does not convert to the interface 'IShape'")) << r.diagnostics;
}

// A native virtual method may implement an interface method when the C++ types are equal (F1).
TEST(ScriptInterfaceTranslation, NativeOverrideWithEqualTypesImplements)
{
    const hotc::TranslateResult r = run(std::string(kShape)
        + "class Sq : Nat, IShape {\n    override int Area() { return 4; }\n    void Scale(int k) { }\n}\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "{\"IShape\", \"Area\", "));
}

// A child of a script parent finds the exact slot of an inherited overload (F2): the row of the
// child equals the row of the parent, and it is the row of Foo(float), not of Foo(int).
TEST(ScriptInterfaceTranslation, InheritedNativeOverloadUsesTheExactSlot)
{
    const hotc::TranslateResult r = run(
        "interface IFoo {\n    int Foo(float x);\n}\n"
        "class P : Nat, IFoo {\n    override int Foo(float x) { return 2; }\n}\n"
        "class C : P {\n}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    std::vector<std::string> rows;
    std::istringstream in(r.cpp);
    std::string line;
    while (std::getline(in, line))
        if (line.find("{\"IFoo\", \"Foo\", ") != std::string::npos)
            rows.push_back(line);
    ASSERT_EQ(rows.size(), 2u) << r.cpp;
    EXPECT_EQ(rows[0], rows[1]);
}
