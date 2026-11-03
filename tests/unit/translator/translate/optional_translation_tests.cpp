// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests for hotc::translate with the types "T?", "T*" and "$T" (design: one mapper, cpp_storage).
#include "hot/hotc_lib.h"
#include "support/check_translate_test_support.h"
#include "support/translator_test_support.h"

#include <gtest/gtest.h>

#include <string>

using namespace translator_test;
using check_translate_test::check_translate;
using check_translate_test::has;

namespace
{

// A script with the struct "Pt" and the class "Foo" that has the members.
hotc::TranslateResult script(const std::string& members)
{
    return translate_text("struct Pt { int x; }\nclass Foo {\n" + members + "}\n");
}

} // namespace

TEST(OptionalTranslation, FieldParameterAndReturn)
{
    const hotc::TranslateResult r =
        script("    int? x;\n    int? Get(int? a) { return a; }\n    Pt? Make(Pt? p) { return p; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "    std::optional<int32_t> x;\n")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "std::optional<int32_t> Foo__Get(")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "hot::In<std::optional<int32_t>> a")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "std::optional<Pt> Foo__Make(")) << r.cpp;
    EXPECT_FALSE(has(r.cpp, "int32_t?")) << r.cpp;
}

TEST(OptionalTranslation, NullComparisonAndAssignment)
{
    const hotc::TranslateResult r = script("    int? x;\n"
                                           "    void F() {\n"
                                           "        int? y = null;\n"
                                           "        if (y == null) { y = 3; }\n"
                                           "        if (y != null) { x = y; }\n"
                                           "        x = null;\n"
                                           "        if (null == x) { x = 1; }\n"
                                           "    }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "std::optional<int32_t> y = std::nullopt;")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "if (y == std::nullopt) {")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "y = 3;")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "if (y != std::nullopt) {")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "x = y;")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "x = std::nullopt;")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "std::nullopt == ")) << r.cpp;
    EXPECT_FALSE(has(r.cpp, "y == nullptr")) << r.cpp;
}

TEST(OptionalTranslation, NullInReturnArgumentAndCoalesce)
{
    const hotc::TranslateResult r = script("    int? None() { return null; }\n"
                                           "    void Take(int? a) { }\n"
                                           "    int? F(int? y, int? z) {\n"
                                           "        Take(null);\n"
                                           "        int? a = y ?? null;\n"
                                           "        int? b = y ?? z;\n"
                                           "        int c = y ?? 7;\n"
                                           "        if (y != null && a == null) { return null; }\n"
                                           "        if (!(b != null)) { return b; }\n"
                                           "        return null;\n"
                                           "    }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "return std::optional<int32_t>();")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "Foo__Take(__self, std::nullopt);")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "const std::optional<int32_t> a = y;")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "hot::coalesce(y, [&]() -> auto { return (z); })")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "hot::coalesce(y, [&]() -> auto { return (7); })")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "y != std::nullopt && a == std::nullopt")) << r.cpp;
    EXPECT_FALSE(has(r.cpp, "nullptr)")) << r.cpp;
    EXPECT_FALSE(has(r.cpp, "== nullptr")) << r.cpp;
    EXPECT_FALSE(has(r.cpp, "!= nullptr")) << r.cpp;
}

TEST(OptionalTranslation, MemberAccessChecksTheOptional)
{
    const hotc::TranslateResult r = script("    Pt? p;\n    int F(Pt? q) { Pt? l = q; Pt? m = p; if (l == null || q == null || m == null) return 0; return l.x + q.x + m.x; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "hot::deref(l).x")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "hot::deref(q).x")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, ").x")) << r.cpp;
    EXPECT_FALSE(has(r.cpp, "l->x")) << r.cpp;
    EXPECT_FALSE(has(r.cpp, "q->x")) << r.cpp;
}

TEST(OptionalTranslation, ReturnNullIsTheEmptyOptional)
{
    const hotc::TranslateResult r = script("    int? F() { return null; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "return std::nullopt;")) << r.cpp;
}

TEST(OptionalTranslation, OptionalOfAHandleIsAnError)
{
    EXPECT_TRUE(has(script("    Foo? f;\n").diagnostics, "\"?\" needs a value type")) << script("    Foo? f;\n").diagnostics;
    EXPECT_TRUE(has(script("    void F() { Foo? f; }\n").diagnostics, "\"?\" needs a value type"));
    EXPECT_TRUE(has(script("    void F(Foo? f) { }\n").diagnostics, "\"?\" needs a value type"));
    EXPECT_TRUE(has(script("    void F() { std.vector<Foo?> v; }\n").diagnostics, "\"?\" needs a value type"));
}

TEST(OptionalTranslation, OptionalOfAPointerOrHandleMarkIsAnError)
{
    const std::string pointer = check_translate("", "    void F(Weapon*? w) { }\n").diagnostics;
    EXPECT_TRUE(has(pointer, "already nullable")) << pointer;
    const std::string handle = script("    void F($Foo? h) { }\n").diagnostics;
    EXPECT_TRUE(has(handle, "already nullable")) << handle;
}

TEST(OptionalTranslation, PointerStaysAPointer)
{
    const hotc::TranslateResult r = check_translate("", "    void F(Weapon* p) { Weapon* q = p; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "Weapon* p")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "Weapon* const q = p;")) << r.cpp;
}

TEST(OptionalTranslation, HandleMarkGivesTheSharedPointer)
{
    const hotc::TranslateResult r = script("    $Foo h;\n    void F($Foo a) { $Foo c = a; }\n    $Foo G() { return null; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "    std::shared_ptr<Foo> h;\n")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "const std::shared_ptr<Foo>& a")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "std::shared_ptr<Foo> Foo__G(")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "return nullptr;")) << r.cpp;
}

TEST(OptionalTranslation, HandleMarkOnAStruct)
{
    const hotc::TranslateResult r = script("    $Pt h;\n    void F() { $Pt c = new $Pt(); }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "    std::shared_ptr<Pt> h;\n")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "std::shared_ptr<Pt> c = std::make_shared<Pt>();")) << r.cpp;
}

TEST(OptionalTranslation, HandleMarkIsTheHandleAndTheBareNameIsTheBox)
{
    const std::string body = "    $Foo h;\n    void F() { $Foo c = new $Foo(); if (c is $Foo) { $Foo d = c as $Foo; } }\n";
    std::string bare = body;
    for (size_t p; (p = bare.find('$')) != std::string::npos;)
        bare.erase(p, 1);
    const hotc::TranslateResult marked = script(body);
    const hotc::TranslateResult plain = script(bare);
    ASSERT_TRUE(marked.ok) << marked.diagnostics;
    EXPECT_TRUE(has(marked.cpp, "std::shared_ptr<Foo> c = std::shared_ptr<Foo>(Foo__new_shared());")) << marked.cpp;
    EXPECT_FALSE(has(marked.cpp, "hot::Value<Foo>")) << marked.cpp;
    if (plain.ok)
        EXPECT_TRUE(has(plain.cpp, "hot::Value<Foo>")) << plain.cpp;
}

TEST(OptionalTranslation, SimpleHandleDeclarationMatchesTheBareDeclaration)
{
    const hotc::TranslateResult marked = script("    void F() { $Foo h = new $Foo(); }\n");
    const hotc::TranslateResult plain = script("    void F() { $Foo h = new $Foo(); }\n");
    ASSERT_TRUE(marked.ok) << marked.diagnostics;
    ASSERT_TRUE(plain.ok) << plain.diagnostics;
    EXPECT_EQ(marked.cpp, plain.cpp);
}

TEST(OptionalTranslation, HandleMarkInTemplateArgumentsAndForeach)
{
    const hotc::TranslateResult r = script("    void F(std.vector<$Foo> v) { std.vector<$Foo> w; foreach ($Foo e in v) { w.push_back(e); } }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "std::vector<std::shared_ptr<Foo>> w")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "std::vector<std::shared_ptr<Foo>>")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "std::shared_ptr<Foo> e")) << r.cpp;
}
