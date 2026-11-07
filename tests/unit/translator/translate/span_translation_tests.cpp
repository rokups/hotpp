// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests for hotc::translate with the type "T[]" (a view): one mapper gives hot::span<T> (or std::span<T> for the view type std).
#include "hot/hotc_lib.h"
#include "support/translator_test_support.h"

#include <gtest/gtest.h>

#include <string>

using namespace translator_test;

namespace
{

bool has(const std::string& text, const std::string& part)
{
    return text.find(part) != std::string::npos;
}

// The default view type is hot (hot::span), which works with C++17.
hotc::TranslateResult script(
    const std::string& members, const char* std_version = "c++20", const char* view_type = "hot")
{
    hotc::TranslateOptions o;
    o.script_name = "t.hot";
    o.script_text = "class Foo {\n" + members + "}\n";
    o.generation = 1;
    o.std_version = std_version;
    o.view_type = view_type;
    return hotc::translate(o);
}

} // namespace

TEST(SpanTranslation, ParameterAndReturn)
{
    const hotc::TranslateResult r =
        script("    int Sum(int[] s) { return 0; }\n    int[] Same(int[] s) { return s; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "int32_t Sum(hot::span<int32_t> s);")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "hot::span<int32_t> Same(hot::span<int32_t> s);")) << r.cpp;
    EXPECT_FALSE(has(r.cpp, "int32_t[]")) << r.cpp;
}

TEST(SpanTranslation, SpanOfOptionalElements)
{
    const hotc::TranslateResult r = script("    void F(int?[] s) { }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "hot::span<std::optional<int32_t>> s")) << r.cpp;
}

TEST(SpanTranslation, LocalSpan)
{
    const hotc::TranslateResult r = script("    void F(int[] s) { int[] t = s; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "hot::span<int32_t> t = s;")) << r.cpp;
}

TEST(SpanTranslation, SpanFieldIsAnError)
{
    const hotc::TranslateResult r = script("    int[] s;\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "T[] is a view; a field needs std.vector<T> or T[N]")) << r.diagnostics;
}

TEST(SpanTranslation, SpanPropertyIsAnError)
{
    const hotc::TranslateResult r = script("    int[] S { get; set; }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "T[] is a view")) << r.diagnostics;
}

TEST(SpanTranslation, ForeachAndIndexAndSizeAndEmpty)
{
    const hotc::TranslateResult r = script(
        "    int F(int[] s) {\n"
        "        int t = 0;\n"
        "        foreach (var x in s) { t += x; }\n"
        "        foreach (ref y in s) { y += 1; }\n"
        "        if (s.empty()) { return 0; }\n"
        "        return t + s[0] + s.size();\n"
        "    }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "for (auto __it0 = hot::iter(s); __it0.next();) { auto x = __it0.ref();")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "__it1.ref() += 1")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "hot::index(s, 0)")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "(s).empty()")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "(s).size()")) << r.cpp;
}

TEST(SpanTranslation, LocalViewOfVectorAndArray)
{
    // The view can write the source, so the local source is not const.
    const hotc::TranslateResult r = script(
        "    void F() {\n"
        "        std.vector<int> v = [1, 2];\n"
        "        int[2] a;\n"
        "        int[] s = v;\n"
        "        int[] t = a;\n"
        "        G(v);\n"
        "        G(a);\n"
        "    }\n"
        "    void G(int[] s) { }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    // A view of a named owner is a checked path (B7): the declaration only reads the owner.
    EXPECT_TRUE(has(r.cpp, "(void)(v);")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "(void)(a);")) << r.cpp;
    EXPECT_FALSE(has(r.cpp, "const std::vector<int32_t> v")) << r.cpp;
    EXPECT_FALSE(has(r.cpp, "const std::array<int32_t, 2> a")) << r.cpp;
}

TEST(SpanTranslation, ConstSourceGoesToConstView)
{
    const hotc::TranslateResult r = script(
        "    int Total(const int[] s) { return s.size(); }\n"
        "    int F(in std.vector<int> v, in int[3] a) { return Total(v) + Total(a); }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "hot::span<const int32_t> s")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "const std::vector<int32_t>& v")) << r.cpp;
}

TEST(SpanTranslation, ReturnOfLocalVectorIsAnError)
{
    const hotc::TranslateResult r =
        script("    int[] F() { std.vector<int> v = [1, 2]; return v; }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "points into the local 'v'")) << r.diagnostics;
}

TEST(SpanTranslation, ReturnOfLocalArrayIsAnError)
{
    const hotc::TranslateResult r = script("    int[] F() { int[2] a; return a; }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "points into the local 'a'")) << r.diagnostics;
}

TEST(SpanTranslation, ReturnOfAVarLocalThatOwnsIsAnError)
{
    const hotc::TranslateResult a = script("    int[] F() { var v = std.vector<int>(); return v; }\n");
    EXPECT_FALSE(a.ok);
    EXPECT_TRUE(has(a.diagnostics, "points into the local 'v'")) << a.diagnostics;
    // The type comes from a method that returns a vector, and from the copy of an owning local.
    const hotc::TranslateResult b = script(
        "    std.vector<int> Make() { std.vector<int> v; return v; }\n"
        "    int[] F() { var v = Make(); return v; }\n"
        "    int[] G() { std.vector<int> a; var w = a; return w; }\n");
    EXPECT_FALSE(b.ok);
    EXPECT_TRUE(has(b.diagnostics, "points into the local 'v'")) << b.diagnostics;
    EXPECT_TRUE(has(b.diagnostics, "points into the local 'w'")) << b.diagnostics;
}

TEST(SpanTranslation, ReturnOfAByValueParameterThatOwnsIsAnError)
{
    const hotc::TranslateResult v = script("    int[] F(std.vector<int> v) { return v; }\n");
    EXPECT_FALSE(v.ok);
    EXPECT_TRUE(has(v.diagnostics, "points into the local 'v'")) << v.diagnostics;
    const hotc::TranslateResult a = script("    int[] F(int[3] a) { return a; }\n");
    EXPECT_FALSE(a.ok);
    EXPECT_TRUE(has(a.diagnostics, "points into the local 'a'")) << a.diagnostics;
}

TEST(SpanTranslation, ReturnOfAReferenceParameterIsValid)
{
    const hotc::TranslateResult r = script(
        "    int[] F(ref std.vector<int> v) { return v; }\n"
        "    int[] G(in std.vector<int> v) { return v; }\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

// The owning name of an inner block does not reach a name of the outer scope.
TEST(SpanTranslation, OwningLocalIsScopeAware)
{
    const hotc::TranslateResult ok = script(
        "    int[] F(int[] s) { if (s.size() > 0) { std.vector<int> v; v.push_back(1); } int[] v = s; return v; }\n");
    EXPECT_TRUE(ok.ok) << ok.diagnostics;
    const hotc::TranslateResult shadow = script(
        "    int[] F(int[] s) { std.vector<int> v; if (s.size() > 0) { int[] v = s; return v; } return s; }\n");
    EXPECT_TRUE(shadow.ok) << shadow.diagnostics;
    const hotc::TranslateResult bad = script(
        "    int[] F(int[] s) { if (s.size() > 0) { std.vector<int> v; return v; } return s; }\n");
    EXPECT_FALSE(bad.ok);
}

TEST(SpanTranslation, ReturnOfParameterViewIsValid)
{
    const hotc::TranslateResult r = script(
        "    int[] F(int[] s) { int[] t = s; return t; }\n"
        "    std.vector<int> G() { std.vector<int> v = [1]; return v; }\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

TEST(SpanTranslation, ViewWorksWithCxx17)
{
    const hotc::TranslateResult r = script(
        "    int Sum(int[] s) { return s[0]; }\n"
        "    void F() { std.vector<int> v = [1, 2]; int[] t = v; Sum(t); }\n",
        "c++17");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "int32_t Sum(hot::span<int32_t> s);")) << r.cpp;
    EXPECT_FALSE(has(r.cpp, "std::span<")) << r.cpp;
}

TEST(SpanTranslation, ViewTypeHotGivesHotSpan)
{
    const hotc::TranslateResult r = script("    void F(int[] s, const int[] c) { }\n", "c++20", "hot");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "hot::span<int32_t> s")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "hot::span<const int32_t> c")) << r.cpp;
    EXPECT_FALSE(has(r.cpp, "std::span<")) << r.cpp;
}

TEST(SpanTranslation, ViewTypeStdGivesStdSpan)
{
    const hotc::TranslateResult r = script(
        "    int[] F(int[] s) { int[] t = s; return t; }\n"
        "    void G(const int[] c) { }\n",
        "c++20", "std");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "std::span<int32_t> F(std::span<int32_t> s);")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "std::span<int32_t> t = s;")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "std::span<const int32_t> c")) << r.cpp;
    EXPECT_FALSE(has(r.cpp, "hot::span<")) << r.cpp;
}

TEST(SpanTranslation, ViewTypeStdNeedsCxx20)
{
    const hotc::TranslateResult r = script("    void F(int[] s) { }\n", "c++17", "std");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "needs C++20")) << r.diagnostics;
}

TEST(SpanTranslation, UnknownViewTypeIsAnError)
{
    const hotc::TranslateResult r = script("    void F(int[] s) { }\n", "c++20", "span");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "view type")) << r.diagnostics;
}

// A view that is last used before the shape change of its source is valid.
TEST(SpanTranslation, ViewUsedBeforeShapeChangeIsValid)
{
    const hotc::TranslateResult r = script(
        "    std.vector<int> xs;\n"
        "    int F() {\n"
        "        std.vector<int> v = [1, 2, 3];\n"
        "        int[] s = v;\n"
        "        int first = s[0];\n"
        "        v.push_back(first);\n"
        "        return first;\n"
        "    }\n"
        "    int G() {\n"
        "        int[] s = this.xs;\n"
        "        int n = s.size();\n"
        "        this.xs.clear();\n"
        "        return n;\n"
        "    }\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

// A view of a named owner is a path: it cannot take a new value.
TEST(SpanTranslation, ViewOfANamedOwnerCannotTakeANewValue)
{
    const hotc::TranslateResult r = script(
        "    int F() {\n"
        "        std.vector<int> v = [1, 2, 3];\n"
        "        int[] s = v;\n"
        "        v.push_back(4);\n"
        "        s = v;\n"
        "        int[] t = v;\n"
        "        return s[0] + t[0];\n"
        "    }\n");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "the view 's' of a named owner cannot take a new value")) << r.diagnostics;
}

TEST(SpanTranslation, ViewOfAnotherContainerSurvivesTheShapeChange)
{
    const hotc::TranslateResult r = script(
        "    int F() {\n"
        "        std.vector<int> v = [1, 2, 3];\n"
        "        std.vector<int> w = [4];\n"
        "        int[] s = v;\n"
        "        w.push_back(5);\n"
        "        return s[0];\n"
        "    }\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

// A view of a named owner is a checked path: each use takes the view again (B7).
TEST(SpanTranslation, ViewAfterAnInnerBlockChangeIsAPath)
{
    const hotc::TranslateResult r = script(
        "    int F(bool b) {\n"
        "        std.vector<int> v = [1, 2, 3];\n"
        "        int[] s = v;\n"
        "        if (b) { v.clear(); }\n"
        "        return s[0];\n"
        "    }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "hot::index((hot::span<int32_t>(v)), 0)")) << r.cpp;
}

TEST(SpanTranslation, ViewReturnedFromARefParameterOrAViewParameterIsValid)
{
    const hotc::TranslateResult r = script(
        "    int[] F(ref std.vector<int> v) { int[] s = v; return s; }\n"
        "    int[] G(int[] p) { int[] s = p; return s; }\n"
        "    std.string_view H(std.string_view p) { return p; }\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

TEST(SpanTranslation, StringViewFieldWithALiteralIsValid)
{
    const hotc::TranslateResult r = script("    std.string_view Name = \"literal\";\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
    const hotc::TranslateResult bad = script("    std.string_view name;\n");
    EXPECT_FALSE(bad.ok);
    EXPECT_TRUE(has(bad.diagnostics, "cannot be stored in a field")) << bad.diagnostics;
}

// A native function may keep a view or a ref. The checker cannot see it, so it accepts the call.
TEST(SpanTranslation, ViewOrRefLocalFromARefParameterIsValid)
{
    const hotc::TranslateResult r = script(
        "    int F(ref int r) { ref q = r; return q; }\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

// The statement reads its own operands before the change takes effect.
TEST(SpanTranslation, ViewReadInTheStatementThatChangesTheSourceIsValid)
{
    const hotc::TranslateResult r = script(
        "    int F() {\n"
        "        std.vector<int> v = [1, 2, 3];\n"
        "        int[] s = v;\n"
        "        v.push_back(s[0]);\n"
        "        return v.size();\n"
        "    }\n"
        "    std.string G() {\n"
        "        std.string t = \"abc\";\n"
        "        std.string_view w = t;\n"
        "        t = std.string(w.substr(1));\n"
        "        return t;\n"
        "    }\n"
        "    std.string H() {\n"
        "        std.string t = \"abc\";\n"
        "        std.string_view w = t;\n"
        "        t += w;\n"
        "        return t;\n"
        "    }\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

TEST(SpanTranslation, ViewUsedAfterTheStatementThatChangesTheSourceIsAPath)
{
    const hotc::TranslateResult r = script(
        "    int F() {\n"
        "        std.vector<int> v = [1, 2, 3];\n"
        "        int[] s = v;\n"
        "        v.push_back(s[0]);\n"
        "        return s[1];\n"
        "    }\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

// A block that ends with a jump never reaches the code after it.
TEST(SpanTranslation, ChangeInABlockThatEndsWithAJumpKeepsTheViewValid)
{
    const hotc::TranslateResult r = script(
        "    int F(bool c) {\n"
        "        std.vector<int> v = [1, 2, 3];\n"
        "        int[] s = v;\n"
        "        if (c) { v.push_back(1); return 0; }\n"
        "        return s[0];\n"
        "    }\n"
        "    int G() {\n"
        "        std.vector<int> v = [1, 2, 3];\n"
        "        int[] s = v;\n"
        "        int n = 0;\n"
        "        for (int i = 0; i < 3; i++) {\n"
        "            if (i == 1) { v.clear(); break; }\n"
        "            n += s[i];\n"
        "        }\n"
        "        return n;\n"
        "    }\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
}

TEST(SpanTranslation, ViewInAFunctionTypeOfAFieldIsNotStorage)
{
    const hotc::TranslateResult r = script("    std.function<void(int[])> OnData;\n");
    EXPECT_TRUE(r.ok) << r.diagnostics;
    const hotc::TranslateResult bad = script("    std.vector<std.string_view> Names;\n");
    EXPECT_FALSE(bad.ok);
}

// G22: a view "T[]" as the argument of a native parameter of a span type. The argument is
// "hot::span_as(v)": it converts to eastl::span<T>, std::span<T> and span<const T>.
TEST(SpanTranslation, ViewToNativeSpanParameter)
{
    translator_test::TempDir dir;
    translator_test::write_file(dir.path() / "w.h",
        "#include <cstddef>\n#include <span>\n"
        "namespace eastl { template <class T> class span { public: span() {}\n"
        "    span(T* p, std::size_t n) : p_(p), n_(n) {} T* p_ = nullptr; std::size_t n_ = 0; }; }\n"
        "inline int SumE(eastl::span<int> s) { return 0; }\n"
        "inline int SumS(std::span<int> s) { return 0; }\n"
        "inline int SumC(std::span<const int> s) { return 0; }\n"
        "inline int SumR(const eastl::span<const int>& s) { return 0; }\n");
    hotc::TranslateOptions o = translator_test::temp_options(dir,
        "using \"w.h\";\n"
        "int F(int[] v) { return SumE(v) + SumS(v) + SumC(v) + SumR(v); }\n");
    o.std_version = "c++20";
    const hotc::TranslateResult r = hotc::translate(o);
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "SumE(hot::span_as(v))")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "SumS(hot::span_as(v))")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "SumC(hot::span_as(v))")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "SumR(hot::span_as(v))")) << r.cpp;
}
