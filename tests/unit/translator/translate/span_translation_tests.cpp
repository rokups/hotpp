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
    EXPECT_TRUE(has(r.cpp, "for (auto x : s)")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "for (auto& y : s)")) << r.cpp;
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
    EXPECT_TRUE(has(r.cpp, "hot::span<int32_t> s = v;")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "hot::span<int32_t> t = a;")) << r.cpp;
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
