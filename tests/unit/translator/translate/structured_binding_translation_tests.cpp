// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests for the translation of the structured bindings and the foreach helpers.
#include "hot/hotc_lib.h"
#include "support/translator_test_support.h"

#include <gtest/gtest.h>

#include <string>

namespace
{

using namespace translator_test;

// A header with the standard types of the scripts.
const char* kHeader = "#include <map>\n#include <utility>\n#include <vector>\n";

hotc::TranslateResult translate_with_header(const std::string& text)
{
    TempDir dir;
    write_file(dir.path() / "u.h", kHeader);
    return hotc::translate(temp_options(dir, text));
}

bool has(const std::string& text, const std::string& part)
{
    return text.find(part) != std::string::npos;
}

// A class with the given body for the method Run.
std::string script(const std::string& body)
{
    return "using \"u.h\";\n"
           "class A {\n    std.pair<int, int> pr;\n    std.map<int, int> m;\n"
           "    std.pair<int, int> Mk() { return std.make_pair(1, 2); }\n"
           "    void Run() { "
        + body + " }\n}\n";
}

} // namespace

TEST(StructuredBindingTranslation, LocalForms)
{
    const hotc::TranslateResult r = translate_with_header(
        script("var (a, b) = Mk(); ref (c, d) = this.pr; var (e, f) = Mk(); int s = a + b + c + d + e + f;"));
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "auto [a, b] = A__Mk(__self);")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "auto& [c, d] = __st->pr;")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "auto [e, f] = A__Mk(__self);")) << r.cpp;
}

TEST(StructuredBindingTranslation, ForeachForms)
{
    const hotc::TranslateResult r = translate_with_header(
        script("int s = 0; foreach (var (k, v) in this.m) s += k + v; foreach (ref (k, v) in this.m) v += 1;"
               " foreach (var (i, x) in enumerate(this.m)) s += x; foreach (int i in range(3)) s += i;"
               " foreach (int i in range(1, 3)) s += i;"));
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "for (auto [k, v] : __st->m)")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "for (auto& [k, v] : __st->m)")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "for (auto [i, x] : hot::enumerate(__st->m))")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "for (int32_t i : hot::range(3))")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "for (int32_t i : hot::range(1, 3))")) << r.cpp;
}

TEST(StructuredBindingTranslation, NamesAreLocals)
{
    // A binding name can shadow nothing and is not borrowed: a lambda may capture it.
    const hotc::TranslateResult r = translate_with_header(script("var (a, b) = Mk(); var f = () => a + b;"));
    ASSERT_TRUE(r.ok) << r.diagnostics;
}

TEST(StructuredBindingTranslation, RefBindingIsBorrowedForALambda)
{
    const hotc::TranslateResult r = translate_with_header(script("ref (c, d) = this.pr; var f = () => c + d;"));
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "the lambda captures 'c', a borrowed pointer or reference")) << r.diagnostics;
}

TEST(StructuredBindingTranslation, ForeachRefBindingIsBorrowedForALambda)
{
    const hotc::TranslateResult r =
        translate_with_header(script("foreach (ref (k, v) in this.m) { var f = () => k + v; }"));
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "the lambda captures 'k', a borrowed pointer or reference")) << r.diagnostics;
}

TEST(StructuredBindingTranslation, MalformedBindingIsAnError)
{
    hotc::TranslateResult r = translate_with_header(script("var (a) = Mk();"));
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "a structured binding needs two or more names")) << r.diagnostics;
    r = translate_with_header(script("var (a, b);"));
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "a structured binding needs an initializer")) << r.diagnostics;
}

TEST(StructuredBindingTranslation, RefBindingCannotBindToATemporaryRange)
{
    hotc::TranslateResult r =
        translate_with_header(script("int s = 0; foreach (ref (i, x) in enumerate(this.m)) s += x;"));
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "'ref' cannot bind to the temporary value of 'enumerate'; use 'var (i, x)'"))
        << r.diagnostics;
    r = translate_with_header(script("int s = 0; foreach (ref i in range(3)) s += i;"));
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(has(r.diagnostics, "'ref' cannot bind to the temporary value of 'range'; use 'var i'"))
        << r.diagnostics;
}

namespace
{

// The helpers "enumerate" and "range" are the last choice of a bare call. A member, a local, a
// parameter and a native global of the same name win.
TEST(StructuredBindingTranslation, UserDefinedRangeWinsOverTheHelper)
{
    hotc::TranslateResult r = translate_with_header(
        "using \"u.h\";\n"
        "class A {\n    int range(int n) { return n + 1; }\n"
        "    int Run() { return range(2); }\n}\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "return A__range(__self, 2);")) << r.cpp;
    EXPECT_FALSE(has(r.cpp, "hot::range")) << r.cpp;

    r = translate_with_header(script("var range = 3; var enumerate = 4; int s = range + enumerate;"));
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_FALSE(has(r.cpp, "hot::range")) << r.cpp;
    EXPECT_FALSE(has(r.cpp, "hot::enumerate")) << r.cpp;
    r = translate_with_header("using \"u.h\";\nclass A {\n    int Run(int range) { return range(1); }\n}\n");
    EXPECT_FALSE(has(r.cpp, "hot::range")) << r.cpp;

    TempDir dir;
    write_file(dir.path() / "g.h", "inline int range(int n) { return n; }\n");
    r = hotc::translate(temp_options(dir, "using \"g.h\";\nclass A {\n    int Run() { return range(3); }\n}\n"));
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "return range(3);")) << r.cpp;
    EXPECT_FALSE(has(r.cpp, "hot::range")) << r.cpp;
}

} // namespace

// C++17 cannot capture a structured binding in a lambda. A lambda, a "??" and a "finally" that
// use a binding get an init-capture. A name after "." and a lambda parameter get no capture.
TEST(StructuredBindingTranslation, LambdaCapturesBindingsByInitCapture)
{
    const hotc::TranslateResult r = translate_with_header(
        script("var (a, b) = Mk(); var fn = () => a + b; int s = fn();"
               " foreach (var (k, v) in this.m) { var g = (int a) => a + v + this.pr.first; s += g(1); }"
               " try { s += a; } finally { s += b; }"));
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "[=, a = a, b = b]()")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "[=, v = v](int32_t a)")) << r.cpp;
    EXPECT_TRUE(has(r.cpp, "([&, &b = b] {")) << r.cpp;
}
