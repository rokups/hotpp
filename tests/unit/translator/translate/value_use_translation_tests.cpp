// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests for hotc::translate with a bare script class, the value use (checker rules).
#include "hot/hotc_lib.h"
#include "support/stub_translate_test_support.h"
#include "support/translator_test_support.h"

#include <gtest/gtest.h>

#include <string>

using namespace translator_test;
using namespace stub_translate_test;

namespace
{

// A script after the Weapon header and stub. "Pt" is a script class with no base and no
// constructor. "Cfg" has only a constructor with a parameter. "Boss" has the native base Widget.
const char* kClasses =
    "class Pt { int x = 1; }\n"
    "class Cfg { int n; Cfg(int n) { this.n = n; } }\n"
    "class Opt { int n; Opt(int n = 2) { this.n = n; } }\n"
    "class Boss : Widget { int hp; }\n"
    "class Elite : Boss { int rank; }\n"
    "struct Stats { int a; }\n";

hotc::TranslateResult translate_value(const std::string& body)
{
    static TempDir* dir = nullptr;
    if (!dir)
        dir = new TempDir();
    write_file(dir->path() / "w.h", kWeaponHeader);
    return hotc::translate(temp_options(*dir, "using \"w.h\";\n" + std::string(kWeaponStub) + kClasses + body));
}

// The script must fail and the diagnostics must have the exact message.
void expect_error(const std::string& body, const std::string& message)
{
    const hotc::TranslateResult r = translate_value(body);
    EXPECT_FALSE(r.ok) << body;
    EXPECT_TRUE(has(r.diagnostics, message)) << body << "\n" << r.diagnostics;
}

} // namespace

TEST(ValueUseTranslation, ValidUsesAreAccepted)
{
    const hotc::TranslateResult r = translate_value(
        "class Holder {\n"
        "    Pt a;\n"
        "    Pt b = new $Pt();\n"
        "    Opt c;\n"
        "    void F(Pt p) { }\n"
        "    Pt G() { return new $Pt(); }\n"
        "    void H() { Pt l; Pt m = new $Pt(); Opt o; }\n"
        "}\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

TEST(ValueUseTranslation, NativeBaseIsAnError)
{
    const std::string msg = "a script class with a native base cannot be a value; write new $C(...)";
    expect_error("class H {\n    Boss b = new $Boss();\n}\n", msg);
    // The native base can be far up the parent chain.
    expect_error("class H {\n    Elite e = new $Elite();\n}\n", msg);
    expect_error("class H {\n    void F(Boss b) { }\n}\n", msg);
    expect_error("class H {\n    void F() { Boss b = new Boss(); }\n}\n", msg);
    // A handle of the class is valid.
    const hotc::TranslateResult r = translate_value("class H {\n    $Boss b = new $Boss();\n}\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

TEST(ValueUseTranslation, NativeAndStructTypesAreAccepted)
{
    const hotc::TranslateResult r = translate_value(
        "class H {\n    Weapon w;\n    Stats s;\n    int n;\n    Vec3 v;\n"
        "    void F(int n) { Weapon l = null; }\n    Stats G() { return Stats(); }\n}\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

TEST(ValueUseTranslation, ValueCannotBeNull)
{
    const std::string msg = "a value cannot be null; write $C w";
    expect_error("class H {\n    void F() { Pt p = null; }\n}\n", msg);
    expect_error("class H {\n    Pt p;\n    void F() { p = null; }\n}\n", msg);
    expect_error("class H {\n    Pt F() { return null; }\n}\n", msg);
    expect_error("class H {\n    void F() { Stats s = null; }\n}\n", msg);
    const hotc::TranslateResult r = translate_value("class H {\n    void F() { $Pt p = null; }\n}\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

TEST(ValueUseTranslation, VarOfNullHasNoType)
{
    expect_error("class H {\n    void F() { var x = null; }\n}\n", "null has no type; write the type");
}

TEST(ValueUseTranslation, NeedsAConstructorWithNoParameters)
{
    const std::string tail = " has no initializer, so the class needs a constructor with no parameters. Add one, "
                             "or write an initializer";
    expect_error("class H {\n    Cfg c;\n}\n", "a value of the class 'Cfg' in field 'c'" + tail);
    expect_error("class H {\n    void F() { Cfg c; }\n}\n", "a value of the class 'Cfg' in local 'c'" + tail);
    // An initializer or a parameter needs no default constructor.
    const hotc::TranslateResult r = translate_value(
        "class H {\n    Cfg c = new Cfg(1);\n    void F(Cfg p) { Cfg d = new Cfg(2); }\n}\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

TEST(ValueUseTranslation, ModuleVariableIsNotSupported)
{
    expect_error("Pt g = new Pt();\n", "a module variable cannot hold a value of a script class; write $Pt g");
}

TEST(ValueUseTranslation, FieldOfAStructIsNotSupported)
{
    expect_error("struct Wrap {\n    Pt p = new Pt();\n}\n",
        "the field 'p' of the struct 'Wrap' cannot hold a value of a script class; write $Pt p");
}

TEST(ValueUseTranslation, LambdaParameterIsAccepted)
{
    const hotc::TranslateResult r = translate_value("class H {\n    void F() { var f = (Pt x) => 0; }\n}\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

TEST(ValueUseTranslation, ReferenceIsAnError)
{
    const std::string msg = "a reference to a value of a script class is not supported; write $Pt";
    expect_error("class H {\n    void F(ref Pt p) { }\n}\n", msg);
    expect_error("class H {\n    void F(out Pt p) { }\n}\n", msg);
}

// An "in" parameter of a script class is the plain box form: a const reference to the value.
TEST(ValueUseTranslation, InParameterIsTheBoxForm)
{
    const hotc::TranslateResult r = translate_value("class H {\n    void F(in Pt p) { }\n}\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

TEST(ValueUseTranslation, ConstByValueIsJudgedAsTheClass)
{
    const hotc::TranslateResult r = translate_value(
        "class H {\n    void F(const Pt p) { Pt c = p; }\n}\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
    // The class rules still apply to the const form.
    expect_error("class H {\n    void F(const Boss p) { }\n}\n",
        "a script class with a native base cannot be a value; write new $C(...)");
}

TEST(ValueUseTranslation, OverrideMustMatchTheSpelling)
{
    const std::string base = "class Base {\n    virtual void F($Pt p) { }\n    virtual $Pt G() { return new $Pt(); }\n}\n";
    expect_error(base + "class Kid : Base {\n    override void F(Pt p) { }\n}\n",
        "'F' has other parameter types than the virtual method of Base; write 'new' to hide it");
    expect_error(base + "class Kid : Base {\n    override Pt G() { return new Pt(); }\n}\n",
        "return type 'Pt' differs from '$Pt'");
    const hotc::TranslateResult r = translate_value(
        "class Base {\n    virtual void F(Pt p) { }\n}\n"
        "class Kid : Base {\n    override void F(Pt p) { }\n}\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

namespace
{

// True when the C++ text has "needle".
bool emits(const hotc::TranslateResult& r, const std::string& needle)
{
    return r.cpp.find(needle) != std::string::npos;
}

} // namespace

TEST(ValueUseLowering, FieldsParametersAndReturnsHaveTheBoxType)
{
    const hotc::TranslateResult r = translate_value(
        "class H {\n"
        "    Pt a;\n"
        "    Pt b = new $Pt();\n"
        "    int Sum(Pt p) { return p.x; }\n"
        "    Pt Make() { return new $Pt(); }\n"
        "}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(emits(r, "hot::Value<Pt> a;")) << r.cpp;
    EXPECT_TRUE(emits(r, "hot::Value<Pt> b;"));
    EXPECT_TRUE(emits(r, "int32_t Sum(hot::In<hot::Value<Pt>> p)"));
    EXPECT_TRUE(emits(r, "hot::Value<Pt> Make()"));
    // A field copy function is in the table. The reload moves the box without a copy.
    EXPECT_TRUE(emits(r, "hot::field_copy<hot::Value<Pt>>()"));
}

TEST(ValueUseLowering, NewGoesIntoTheBoxAndAHandleIsCopied)
{
    const hotc::TranslateResult r = translate_value(
        "class H {\n"
        "    Pt a;\n"
        "    $Pt h = new $Pt();\n"
        "    Pt Dup() { return h; }\n"
        "    void F() { a = new $Pt(); a = h; }\n"
        "}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    // The default of a field runs "new Pt()", and "new" is not copied.
    EXPECT_TRUE(emits(r, "hot::Value<Pt>(std::shared_ptr<Pt>(Pt__new_shared()))")) << r.cpp;
    // A handle source goes through copy_of (as_value): a null handle throws there.
    EXPECT_TRUE(emits(r, "hot::as_value<Pt>(h)")) << r.cpp;
    // A function that returns a box throws in a stale call. A box has no empty state.
    EXPECT_TRUE(emits(r, "hot::detail::throw_null_reference(\"call of a retired generation"));
}

TEST(ValueUseLowering, LocalsAreBoxesAndVarCopies)
{
    const hotc::TranslateResult r = translate_value(
        "class H {\n"
        "    Pt a;\n"
        "    int F() {\n"
        "        Pt z;\n"
        "        Pt l = new $Pt();\n"
        "        var m = a;\n"
        "        var f = () => m.x + l.x;\n"
        "        return z.x + f();\n"
        "    }\n"
        "}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(emits(r, "hot::Value<Pt> z = hot::Value<Pt>(")) << r.cpp;
    EXPECT_TRUE(emits(r, "hot::Value<Pt> l = hot::Value<Pt>("));
    EXPECT_TRUE(emits(r, "auto m = a;")); // the copy constructor of the box copies the object
    EXPECT_TRUE(emits(r, "[=]()")); // the lambda captures the boxes by copy
}

TEST(ValueUseLowering, ModuleSetsTheCallerRecordOnlyWhenItUsesValue)
{
    const hotc::TranslateResult with = translate_value("class H {\n    Pt a;\n}\n");
    ASSERT_TRUE(with.ok) << with.diagnostics;
    // The copies of the box note the generation of this module (V7).
    EXPECT_TRUE(emits(with, "hot::set_value_generation(GEN);")) << with.cpp;
    const hotc::TranslateResult without = translate_value("class H {\n    $Pt a;\n}\n");
    ASSERT_TRUE(without.ok) << without.diagnostics;
    EXPECT_FALSE(emits(without, "set_value_generation"));
}

// A free function has no class: a call of it still gives a box (the rules of the type pass).
TEST(ValueUseTranslation, BoxFromAFreeFunctionHasNoIdentity)
{
    expect_error("Pt Make() { return new $Pt(); }\n"
                 "bool Same() { var x = Make(); var y = Make(); return x == y; }\n",
        "'==' is not valid for a value of a script class");
}

// A static method call gives a box too.
TEST(ValueUseTranslation, BoxFromAStaticMethodHasNoIdentity)
{
    expect_error("class H {\n    static Pt Make() { return new $Pt(); }\n}\n"
                 "bool Same() { var x = H.Make(); var y = H.Make(); return x == y; }\n",
        "'==' is not valid for a value of a script class");
}

// A module with only free functions has no class record, but its copies of a box must still name
// the generation of the module. The call uses the generation constant.
TEST(ValueUseLowering, FreeFunctionModuleSetsTheValueGeneration)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kWeaponHeader);
    hotc::TranslateOptions o = temp_options(dir, "using \"base.hot\";\nint F(Pt a) { var b = a; return b.x; }\n");
    o.modules.emplace_back("base.hot", "class Pt { int x = 1; }\n");
    const hotc::TranslateResult r = hotc::translate(o);
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(emits(r, "hot::set_value_generation(GEN);")) << r.cpp;
}

TEST(ValueUseLowering, BoxToHandleIsAnError)
{
    expect_error("class H {\n    Pt a;\n    void F() { $Pt h = a; }\n}\n",
        "a value cannot convert to a handle: the box owns the object. Declare the target without '$' to copy it");
}

TEST(ValueUseLowering, ForeachWithABareElementIsABox)
{
    const hotc::TranslateResult r = translate_value(
        "class H {\n    int F(std.vector<Pt> v) { int s = 0; foreach (Pt p in v) { s += p.x; } return s; }\n}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(emits(r, "hot::Value<Pt> p")) << r.cpp;
}

TEST(ValueUseTranslation, ForeachOverHandlesNeedsAHandleElement)
{
    expect_error("class H {\n    void F(std.vector<$Pt> v) { foreach (Pt p in v) { } }\n}\n",
        "foreach element type Pt does not match $Pt; write $Pt p");
    const hotc::TranslateResult r =
        translate_value("class H {\n    void F(std.vector<$Pt> v) { foreach ($Pt p in v) { } }\n}\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
    expect_error("class H {\n    void F(std.vector<$Boss> v) { foreach (Boss b in v) { } }\n}\n",
        "a script class with a native base cannot be a value; write new $C(...)");
}

TEST(ValueUseLowering, LambdaParameterIsABox)
{
    const hotc::TranslateResult r = translate_value("class H {\n    void F() { var f = (Pt x) => x.x; }\n}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(emits(r, "hot::Value<Pt> x")) << r.cpp;
    expect_error("class H {\n    void F() { var f = (Boss x) => 0; }\n}\n",
        "a script class with a native base cannot be a value; write new $C(...)");
}

TEST(ValueUseLowering, StructHandleParameterThatIsAssignedIsByValue)
{
    const hotc::TranslateResult r =
        translate_value("class H {\n    void F($Stats p) { p = new $Stats(); }\n}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    // The body takes a copy of the handle, so the assignment does not write a const reference.
    EXPECT_TRUE(emits(r, "std::shared_ptr<Stats> p = std::forward<const std::shared_ptr<Stats>&>(__p)")) << r.cpp;
}

TEST(ValueUseLowering, ByValHandleParameterIsCopied)
{
    const hotc::TranslateResult r = translate_value(
        "class H {\n    void F([byval] $Pt p) { }\n    void G($Pt p) { }\n    void K([byval] Vec3 v) { }\n}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(emits(r, "void F(std::shared_ptr<Pt> p)")) << r.cpp;
    EXPECT_TRUE(emits(r, "void G(const std::shared_ptr<Pt>& p)")) << r.cpp;
    EXPECT_TRUE(emits(r, "void K(Vec3 v)")) << r.cpp;
}

TEST(ValueUseTranslation, BareScriptClassInATemplateArgumentIsABox)
{
    const hotc::TranslateResult r = translate_value("class H {\n    std.vector<Pt> v;\n}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(emits(r, "std::vector<hot::Value<Pt>> v")) << r.cpp;
    const std::string msg = "a script class with a native base cannot be a value; write new $C(...)";
    expect_error("class H {\n    std.vector<Boss> v;\n}\n", msg);
    expect_error("class H {\n    void F(std.vector<Elite> v) { }\n}\n", msg);
    expect_error("class H {\n    void F() { std.vector<Boss> v; }\n}\n", msg);
    const hotc::TranslateResult ok = translate_value("class H {\n    std.vector<$Boss> v;\n}\n");
    EXPECT_TRUE(ok.ok) << ok.diagnostics;
}

TEST(ValueUseTranslation, AsAndIsNeedTheHandleSpelling)
{
    expect_error("class H {\n    $Pt h;\n    bool F() { return h is Pt; }\n}\n", "'is Pt' names a value; write 'is $Pt'");
    expect_error("class H {\n    $Pt h;\n    $Pt F() { return h as Pt; }\n}\n", "'as Pt' names a value; write 'as $Pt'");
    const hotc::TranslateResult r = translate_value(
        "class H {\n    $Pt h;\n    bool F() { return h is $Pt; }\n    $Pt G() { return h as $Pt; }\n}\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

TEST(ValueUseTranslation, BareScriptClassIsNotACast)
{
    const hotc::TranslateResult r =
        translate_value("class H {\n    $Pt h;\n    $Pt F() { return (Pt)h; }\n}\n");
    EXPECT_FALSE(r.ok) << r.cpp;
}

TEST(ValueUseLowering, UsingAliasOfABareScriptClassIsTheBox)
{
    const hotc::TranslateResult r = translate_value("using P2 = Pt;\nclass H {\n    P2 a;\n}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(emits(r, "using P2 = hot::Value<Pt>;")) << r.cpp;
}

// hot::In<T> needs sizeof(T). A struct is incomplete in its own member declarations, so a struct
// parameter goes by "const T&". An array is a std::array: it never becomes a C array parameter.
TEST(ValueUseTranslation, StructParameterOfItsOwnTypeIsAConstReference)
{
    const hotc::TranslateResult r = translate_text(
        "struct A { int x; bool same(A o) { return o.x == x; } bool maybe(A? o) { return o != null; } "
        "int sum(int[3] a) { return a[0]; } }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_NE(r.cpp.find("bool same(const A& o)"), std::string::npos) << r.cpp;
    EXPECT_NE(r.cpp.find("const std::optional<A>& o"), std::string::npos) << r.cpp;
    EXPECT_EQ(r.cpp.find("hot::In<A>"), std::string::npos) << r.cpp;
    EXPECT_NE(r.cpp.find("hot::In<std::array<int32_t, 3>> a"), std::string::npos) << r.cpp;
}
