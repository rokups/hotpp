// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests for hotc::translate with the "[value] T" use of a script class (checker rules).
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
        "    [value] Pt a;\n"
        "    [value] Pt b = new Pt();\n"
        "    [value] Opt c;\n"
        "    void F([value] Pt p) { }\n"
        "    [value] Pt G() { return new Pt(); }\n"
        "    void H() { [value] Pt l; [value] Pt m = new Pt(); [value] Opt o; }\n"
        "}\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

TEST(ValueUseTranslation, NativeBaseIsAnError)
{
    const std::string boss = "[value] on field 'b' needs a script class with no native base, and 'Boss' has the "
                             "native base 'Widget'. Remove '[value]'";
    expect_error("class H {\n    [value] Boss b = new Boss();\n}\n", boss);
    // The native base can be far up the parent chain.
    expect_error("class H {\n    [value] Elite e = new Elite();\n}\n",
        "[value] on field 'e' needs a script class with no native base, and 'Elite' has the native base 'Widget'. "
        "Remove '[value]'");
    expect_error("class H {\n    void F([value] Boss b) { }\n}\n",
        "[value] on parameter 'b' needs a script class with no native base, and 'Boss' has the native base "
        "'Widget'. Remove '[value]'");
}

TEST(ValueUseTranslation, NativeHandleTypeIsAnError)
{
    const std::string tail = " is not valid for the native type 'Weapon'. The type rules decide the storage of a "
                             "native type. Remove '[value]'";
    expect_error("class H {\n    [value] Weapon w;\n}\n", "[value] on field 'w'" + tail);
    expect_error("class H {\n    void F() { [value] Weapon w = null; }\n}\n", "[value] on local 'w'" + tail);
}

TEST(ValueUseTranslation, ValueTypeHasNoEffect)
{
    expect_error("class H {\n    [value] Stats s;\n}\n",
        "[value] on field 's' has no effect: 'Stats' is a value type already. Remove '[value]'");
    expect_error("class H {\n    [value] int n;\n}\n",
        "[value] on field 'n' has no effect: 'int' is a value type already. Remove '[value]'");
    expect_error("class H {\n    [value] Vec3 v;\n}\n",
        "[value] on field 'v' has no effect: 'Vec3' is a value type already. Remove '[value]'");
    expect_error("class H {\n    void F([value] int n) { }\n}\n",
        "[value] on parameter 'n' has no effect: 'int' is a value type already. Remove '[value]'");
    expect_error("class H {\n    [value] Stats G() { return Stats(); }\n}\n",
        "[value] on the return type of 'G' has no effect: 'Stats' is a value type already. Remove '[value]'");
}

TEST(ValueUseTranslation, NeedsAConstructorWithNoParameters)
{
    const std::string tail = " has no initializer, so the class 'Cfg' needs a constructor with no parameters. Add "
                             "one, or write an initializer";
    expect_error("class H {\n    [value] Cfg c;\n}\n", "[value] on field 'c'" + tail);
    expect_error("class H {\n    void F() { [value] Cfg c; }\n}\n", "[value] on local 'c'" + tail);
    // An initializer or a parameter needs no default constructor.
    const hotc::TranslateResult r = translate_value(
        "class H {\n    [value] Cfg c = new Cfg(1);\n    void F([value] Cfg p) { [value] Cfg d = new Cfg(2); }\n}\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

TEST(ValueUseTranslation, ModuleVariableIsNotSupportedYet)
{
    expect_error("[value] Pt g = new Pt();\n", "[value] is not supported yet on a module variable");
}

TEST(ValueUseTranslation, FieldOfAStructIsNotSupportedYet)
{
    expect_error("struct Wrap {\n    [value] Pt p = new Pt();\n}\n",
        "[value] on field 'p' of the struct 'Wrap' is not supported yet");
}

TEST(ValueUseTranslation, LambdaParameterIsAnError)
{
    expect_error("class H {\n    void F() { var f = ([value] Pt x) => 0; }\n}\n",
        "attribute 'value' is not valid on a lambda parameter");
}

TEST(ValueUseTranslation, ReferenceIsAnError)
{
    const std::string msg = "[value] on a reference is not valid: a [value] holds its own copy. Remove the "
                            "reference or the '[value]'";
    expect_error("class H {\n    void F([value] Pt& p) { }\n}\n", msg);
    expect_error("class H {\n    void F([value] const Pt& p) { }\n}\n", msg);
    expect_error("class H {\n    [value] Pt& G(Pt p) { return p; }\n}\n", msg);
    expect_error("class H {\n    void F(Pt a) { [value] var& r = a; }\n}\n", msg);
}

TEST(ValueUseTranslation, ConstByValueIsJudgedAsTheClass)
{
    const hotc::TranslateResult r = translate_value(
        "class H {\n    void F([value] const Pt p) { [value] const Pt c = p; }\n}\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
    // The class rules still apply to the const form.
    expect_error("class H {\n    void F([value] const Boss p) { }\n}\n",
        "[value] on parameter 'p' needs a script class with no native base, and 'Boss' has the native base "
        "'Widget'. Remove '[value]'");
}

TEST(ValueUseTranslation, OverrideMustMatchValue)
{
    const std::string base = "class Base {\n    virtual void F(Pt p) { }\n    virtual Pt G() { return new Pt(); }\n}\n";
    expect_error(base + "class Kid : Base {\n    override void F([value] Pt p) { }\n}\n",
        "'F' differs in '[value]' on the parameter 'p' from the virtual method of Base; write '[value]' on both or "
        "on none");
    expect_error(base + "class Kid : Base {\n    [value] override Pt G() { return new Pt(); }\n}\n",
        "'G' differs in '[value]' on the return type from the virtual method of Base; write '[value]' on both or "
        "on none");
    const hotc::TranslateResult r = translate_value(
        "class Base {\n    virtual void F([value] Pt p) { }\n}\n"
        "class Kid : Base {\n    override void F([value] Pt p) { }\n}\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}
