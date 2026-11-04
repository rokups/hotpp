// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests for the access words public, protected and private on the members of a script class.
// The checker enforces them. The generated C++ keeps all members public.
#include "hot/hotc_lib.h"
#include "support/translator_test_support.h"

#include <gtest/gtest.h>

#include <string>

using namespace translator_test;

namespace
{

hotc::TranslateResult translate_classes(const std::string& classes)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kHeaderV1);
    return hotc::translate(temp_options(dir, "using \"w.h\";\n" + classes));
}

void expect_ok(const std::string& classes)
{
    const hotc::TranslateResult r = translate_classes(classes);
    EXPECT_TRUE(r.ok) << classes << "\n" << r.diagnostics;
}

void expect_error(const std::string& classes, const std::string& msg)
{
    const hotc::TranslateResult r = translate_classes(classes);
    EXPECT_FALSE(r.ok) << classes;
    EXPECT_NE(r.diagnostics.find(msg), std::string::npos) << classes << "\n" << r.diagnostics;
}

// A base class with one member of each kind. "access" is the access word of the members.
std::string base_class(const std::string& access)
{
    return "class Base : Widget {\n"
           "    " + access + " int secret = 1;\n"
           "    " + access + " int Twice() { return this.secret * 2; }\n"
           "    " + access + " int Level { get; set; } = 3;\n"
           "}\n";
}

} // namespace

TEST(AccessTranslation, PrivateFieldReadFromAnotherClassIsAnError)
{
    expect_error(base_class("private") + "class Other : Widget { int Peek($Base b) { if (b == null) return 0; return b.secret; } }\n",
        "'secret' is private in 'Base'");
}

TEST(AccessTranslation, PrivateFieldWriteFromAnotherClassIsAnError)
{
    expect_error(base_class("private") + "class Other : Widget { void Poke($Base b) { if (b == null) return; b.secret = 2; } }\n",
        "'secret' is private in 'Base'");
}

TEST(AccessTranslation, ProtectedFieldFromASubclassIsAllowed)
{
    expect_ok(base_class("protected")
        + "class Sub : Base { int Peek() { return this.secret; } int Bare() { return secret; } }\n");
}

TEST(AccessTranslation, ProtectedFieldFromOutsideIsAnError)
{
    expect_error(base_class("protected") + "class Other : Widget { int Peek($Base b) { if (b == null) return 0; return b.secret; } }\n",
        "'secret' is protected in 'Base'");
}

TEST(AccessTranslation, PrivateFieldOfTheParentIsAnErrorInASubclass)
{
    expect_error(base_class("private") + "class Sub : Base { int Peek() { return this.secret; } }\n",
        "'secret' is private in 'Base'");
    expect_error(base_class("private") + "class Sub : Base { int Peek() { return secret; } }\n",
        "'secret' is private in 'Base'");
}

TEST(AccessTranslation, PrivateMethodCallFromAnotherClassIsAnError)
{
    expect_error(base_class("private") + "class Other : Widget { int Go($Base b) { if (b == null) return 0; return b.Twice(); } }\n",
        "'Twice' is private in 'Base'");
    expect_error(
        base_class("private") + "class Sub : Base { int Go() { return Twice(); } }\n", "'Twice' is private in 'Base'");
    expect_error(base_class("private") + "class Sub : Base { int Go() { return this.Twice(); } }\n",
        "'Twice' is private in 'Base'");
}

TEST(AccessTranslation, ProtectedMethodFromASubclassIsAllowedAndFromOutsideIsDenied)
{
    expect_ok(base_class("protected") + "class Sub : Base { int Go() { return Twice() + this.Twice(); } }\n");
    expect_error(base_class("protected") + "class Other : Widget { int Go($Base b) { if (b == null) return 0; return b.Twice(); } }\n",
        "'Twice' is protected in 'Base'");
}

TEST(AccessTranslation, BaseCallOfAPrivateMethodIsAnError)
{
    expect_error(base_class("private") + "class Sub : Base { int Go() { return base.Twice(); } }\n",
        "'Twice' is private in 'Base'");
    expect_ok(base_class("protected") + "class Sub : Base { int Go() { return base.Twice(); } }\n");
}

TEST(AccessTranslation, ProtectedMemberIsUsableOnlyThroughTheTypeOfTheCode)
{
    const std::string base = base_class("protected");
    // Through this and through a D object. A subclass of D cannot be named in D: it is a cycle.
    expect_ok(base + "class D : Base { int A() { return secret + this.secret + Twice() + Level; } }\n");
    expect_ok(base + "class D : Base { int A($D o) { if (o == null) return 0; return o.secret + o.Twice() + o.Level; } }\n");
    // Through a Base object and through a sibling class.
    const std::string expected = "(use it through 'D' or a subclass)";
    expect_error(base + "class D : Base { int A($Base o) { if (o == null) return 0; return o.secret; } }\n",
        "'secret' is protected in 'Base' " + expected);
    expect_error(base + "class D : Base { int A($Base o) { if (o == null) return 0; return o.Twice(); } }\n",
        "'Twice' is protected in 'Base' " + expected);
    expect_error(
        base + "class D : Base { int A($Base o) { if (o == null) return 0; return o.Level; } }\n", "'Level' is protected in 'Base' " + expected);
    expect_error(
        base + "class D : Base { void A($Base o) { if (o == null) return; o.Level = 2; } }\n", "'Level' is protected in 'Base' " + expected);
    expect_error(base + "class D : Base { int A($E o) { if (o == null) return 0; return o.secret; } }\nclass E : Base {}\n",
        "'secret' is protected in 'Base' " + expected);
    expect_error(base + "class D : Base { int A($E o) { if (o == null) return 0; return o.Twice(); } }\nclass E : Base {}\n",
        "'Twice' is protected in 'Base' " + expected);
    // The class that declares the member can use it through an object of its own type.
    expect_ok("class Base : Widget { protected int secret = 1; int Get($Base o) { if (o == null) return 0; return o.secret; } }\n");
    // A base call stays allowed.
    expect_ok(base + "class D : Base { int A() { return base.Twice(); } }\n");
}

TEST(AccessTranslation, SeveralAccessErrorsInOneMethodAreAllReported)
{
    const hotc::TranslateResult r = translate_classes(base_class("private")
        + "class Other : Widget {\n"
          "    int Go($Base b)\n"
          "    {\n"
          "        if (b == null) return 0; int x = b.secret;\n"
          "        b.secret = 2;\n"
          "        return x + b.Twice() + b.Level;\n"
          "    }\n"
          "}\n");
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.diagnostics.find(":10:"), std::string::npos) << r.diagnostics;
    EXPECT_NE(r.diagnostics.find(":11:"), std::string::npos) << r.diagnostics;
    EXPECT_NE(r.diagnostics.find(":12:"), std::string::npos) << r.diagnostics;
    EXPECT_NE(r.diagnostics.find("'Twice' is private in 'Base'"), std::string::npos) << r.diagnostics;
    EXPECT_NE(r.diagnostics.find("'Level' is private in 'Base'"), std::string::npos) << r.diagnostics;
}

TEST(AccessTranslation, AccessErrorsInSeveralMethodsAreAllReported)
{
    const hotc::TranslateResult r = translate_classes(base_class("private")
        + "class Other : Widget {\n"
          "    int A($Base b) { if (b == null) return 0; return b.secret; }\n"
          "    int B($Base b) { if (b == null) return 0; return b.Twice(); }\n"
          "    int C($Base b) { if (b == null) return 0; return b.Level; }\n"
          "}\n");
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.diagnostics.find("'secret' is private in 'Base'"), std::string::npos) << r.diagnostics;
    EXPECT_NE(r.diagnostics.find("'Twice' is private in 'Base'"), std::string::npos) << r.diagnostics;
    EXPECT_NE(r.diagnostics.find("'Level' is private in 'Base'"), std::string::npos) << r.diagnostics;
}

TEST(AccessTranslation, AccessErrorsInSeveralClassesAreAllReported)
{
    const hotc::TranslateResult r = translate_classes(
        "class Counter : Widget {\n"
        "    private int count = 1;\n"
        "    private int Twice() { return this.count * 2; }\n"
        "}\n"
        "class A : Widget { int Go($Counter c) { if (c == null) return 0; return c.count; } }\n"
        "class B : Widget { int Go($Counter c) { if (c == null) return 0; return c.Twice(); } }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.diagnostics.find("'count' is private in 'Counter'"), std::string::npos) << r.diagnostics;
    EXPECT_NE(r.diagnostics.find("'Twice' is private in 'Counter'"), std::string::npos) << r.diagnostics;
}

TEST(AccessTranslation, PrivatePropertyFromAnotherClassIsAnError)
{
    expect_error(base_class("private") + "class Other : Widget { int Peek($Base b) { if (b == null) return 0; return b.Level; } }\n",
        "'Level' is private in 'Base'");
    expect_error(base_class("private") + "class Other : Widget { void Poke($Base b) { if (b == null) return; b.Level = 1; } }\n",
        "'Level' is private in 'Base'");
}

TEST(AccessTranslation, PrivateSetterOnAPublicPropertyHasItsOwnMessage)
{
    const std::string base = "class Base : Widget { int Level { get; private set; } = 3; }\n";
    expect_ok(base + "class Other : Widget { int Peek($Base b) { if (b == null) return 0; return b.Level; } }\n");
    expect_error(base + "class Other : Widget { void Poke($Base b) { if (b == null) return; b.Level = 1; } }\n",
        "the 'set' accessor of 'Level' is private in 'Base'");
    expect_ok(
        "class Base : Widget { int Level { get; private set; } = 3; void Up() { Level = 4; this.Level += 1; } }\n");
}

TEST(AccessTranslation, PrivateConstructorFromOutsideIsAnError)
{
    const std::string base =
        "class Base : Widget {\n    Base() {}\n    private Base(int n) {}\n    $Base Make() { return new $Base(1); "
        "}\n}\n";
    expect_ok(base);
    expect_error(
        base + "class Other : Widget { $Base Go() { return new $Base(2); } }\n", "the constructor is private in 'Base'");
    expect_ok(base + "class Other : Widget { $Base Go() { return new $Base(); } }\n");
}

TEST(AccessTranslation, PrivateBaseConstructorIsAnErrorInASubclass)
{
    const std::string base =
        "class Base : Widget {\n    private Base(int n) {}\n    protected Base(int a, int b) {}\n}\n";
    expect_error(base + "class Sub : Base { Sub() : base(1) {} }\n", "the constructor is private in 'Base'");
    expect_ok(base + "class Sub : Base { Sub() : base(1, 2) {} }\n");
}

TEST(AccessTranslation, OwnClassAndNestedLambdaAreAllowed)
{
    expect_ok(
        "class Base : Widget {\n"
        "    private int secret = 1;\n"
        "    private int Twice() { return secret * 2; }\n"
        "    private int Level { get; set; } = 3;\n"
        "    int Sum($Base other) { if (other == null) return 0; return other.secret + other.Twice() + other.Level + this.secret + Level; }\n"
        "    void Run() { var f = () => { return secret + this.Twice() + Level; }; f(); }\n"
        "}\n");
}

TEST(AccessTranslation, DefaultAccessIsPublicForClassAndStruct)
{
    expect_ok(
        "class Base : Widget { int a = 1; int F() { return 1; } int P { get; set; } = 1; }\n"
        "class Other : Widget { int Go($Base b) { if (b == null) return 0; b.P = b.a; return b.F() + b.P; } }\n");
}

TEST(AccessTranslation, StructMembersFollowTheSameRules)
{
    const std::string st = "struct Stats {\n    private int hp = 1;\n    int Get() { return hp; }\n}\n";
    expect_ok(st);
    expect_error(st + "class Other : Widget { int Peek(Stats s) { return s.hp; } }\n", "'hp' is private in 'Stats'");
}

TEST(AccessTranslation, GeneratedCppKeepsEveryMemberPublic)
{
    const hotc::TranslateResult r = translate_classes(base_class("private"));
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_EQ(r.cpp.find("private:"), std::string::npos) << r.cpp;
    EXPECT_EQ(r.cpp.find("protected:"), std::string::npos) << r.cpp;
}
