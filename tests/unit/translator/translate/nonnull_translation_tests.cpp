// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests for the non-null assertion "x!": the lowering writes hot::nonnull(x, "file:line: 'x' is null").
#include "hot/hotc_lib.h"
#include "support/check_translate_test_support.h"

#include <gtest/gtest.h>

#include <string>

using namespace check_translate_test;

static const std::string kNode = std::string(kDefaultShared)
    + "class Node { $Node next; int v; $Node Self() { return this.next; } int Add(int a) { return a; } void Ping() {}"
      " std.vector<int> arr; }\n";

static hotc::TranslateResult run(const std::string& body)
{
    return check_translate(kNode, body);
}

static void expect_has(const std::string& body, const std::string& part)
{
    const hotc::TranslateResult r = run(body);
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, part)) << "expected: " << part << "\n" << r.cpp;
}

// The "using" line is the first line of the script, so a member line N is the script line N + 3.
TEST(NonNullTranslation, MemberCall)
{
    expect_has("    void A($Node a) { a!.Ping(); }\n", "hot::nonnull(a, \"t.hot:5: 'a' is null\")");
}

TEST(NonNullTranslation, FieldRead)
{
    expect_has("    void A($Node a) { int q = a!.v; }\n", "shell_of<Node_Shell>(hot::nonnull(a, \"t.hot:5: 'a' is null\")");
}

TEST(NonNullTranslation, FieldWrite)
{
    expect_has("    void A($Node a) { a!.v = 1; }\n", "shell_of<Node_Shell>(hot::nonnull(a, \"t.hot:5: 'a' is null\"), \"Node\"))->v = 1;");
}

TEST(NonNullTranslation, CallResult)
{
    expect_has("    $Node G() { return null; }\n    void A() { this.G()!.Ping(); }\n",
        "hot::nonnull(Foo__G(__self), \"t.hot:6: 'this.G()' is null\")");
}

TEST(NonNullTranslation, Index)
{
    expect_has("    void A(std.vector<int>* p) { int q = p![0]; }\n", "hot::index(hot::nonnull(p, \"t.hot:5: 'p' is null\"), 0)");
}

TEST(NonNullTranslation, Argument)
{
    expect_has("    void Take($Node a) {}\n    void A($Node a) { this.Take(a!); }\n",
        "Foo__Take(__self, hot::nonnull(a, \"t.hot:6: 'a' is null\"))");
}

TEST(NonNullTranslation, Statement)
{
    expect_has("    void A($Node a) { a!; }\n", "hot::nonnull(a, \"t.hot:5: 'a' is null\");");
}

TEST(NonNullTranslation, Initializer)
{
    expect_has("    $Node G() { return null; }\n    void A() { $Node x = this.G()!; }\n",
        "= hot::nonnull(Foo__G(__self), \"t.hot:6: 'this.G()' is null\");");
}

TEST(NonNullTranslation, Lambda)
{
    expect_has("    void A($Node a) { var f = () => a!.Ping(); f(); }\n", "hot::nonnull(a, \"t.hot:5: 'a' is null\")");
}

TEST(NonNullTranslation, Chain)
{
    expect_has("    void A($Node a) { int q = a!.Self()!.v; }\n", "'a!.Self()' is null");
}

TEST(NonNullTranslation, UntypedNativeCall)
{
    expect_has("    void A(ref Owner o) { Weapon* w = o.GetWeapon()!; }\n",
        "hot::nonnull(hot::deref(o).GetWeapon(), \"t.hot:5: 'o.GetWeapon()' is null\")");
    expect_has("    void A() { int d = FindWeapon()!.dmg; }\n", "hot::nonnull(FindWeapon(), \"t.hot:5: 'FindWeapon()' is null\")");
}

TEST(NonNullTranslation, GenericContext)
{
    const hotc::TranslateResult r = check_translate(kNode + "T Id<T>(T* p) { return *p!; }\n", "    int M(int* q) { return Id<int>(q); }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, "hot::nonnull(p, \"t.hot:4: 'p' is null\")")) << r.cpp;
}

TEST(NonNullTranslation, LongTextIsTruncatedAndEscaped)
{
    const hotc::TranslateResult r = run("    $Node G(std.string s) { return null; }\n"
                                        "    void A() { $Node x = this.G(\"q\\\"uote and a very long text that goes past sixty bytes in total\")!; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_TRUE(has(r.cpp, R"('this.G(\"q\\\"uote and a very long text that goes past sixty by...' is null")"))
        << r.cpp;
}

TEST(NonNullTranslation, NotEqualIsNotAnAssertion)
{
    const hotc::TranslateResult r = run("    bool A($Node a, $Node b) { return a != b; }\n");
    ASSERT_TRUE(r.ok) << r.diagnostics;
    EXPECT_FALSE(has(r.cpp, "hot::nonnull(a")) << r.cpp;
}

TEST(NonNullTranslation, TypeCheckSeesThroughTheAssertion)
{
    const hotc::TranslateResult ok = run("    int A($Node a) { return a!.Self()!.Add(a!.v); }\n");
    EXPECT_TRUE(ok.ok) << ok.diagnostics;
    // The type of "a!" is the type of "a": the call "y!.Ping()" is a typed script call.
    const hotc::TranslateResult t = run("    void A($Node a) { var y = a!.Self(); y!.Ping(); }\n");
    ASSERT_TRUE(t.ok) << t.diagnostics;
    EXPECT_TRUE(has(t.cpp, "Node__Ping(hot::shell_of<Node_Shell>(hot::nonnull(y,")) << t.cpp;
}

// The message has the base name of the script: no path of the build machine in the output.
TEST(NonNullTranslation, MessageHasNoDirectory)
{
    TempDir dir;
    hotc::TranslateOptions o = temp_options(dir, std::string(kNode) + "class Foo { void A($Node a) { a!.Ping(); } }\n");
    o.script_name = (dir.path() / "sub" / "t.hot").string();
    const hotc::TranslateResult r = hotc::translate(o);
    ASSERT_TRUE(r.ok) << r.diagnostics;
    const size_t at = r.cpp.find("hot::nonnull(a, \"");
    ASSERT_NE(at, std::string::npos) << r.cpp;
    const std::string msg = r.cpp.substr(at, r.cpp.find(')', at) - at);
    EXPECT_NE(msg.find("\"t.hot:"), std::string::npos) << msg;
    EXPECT_EQ(msg.find('/'), std::string::npos) << msg;
}

// A context that the lowering does not visit as a normal expression gives a diagnostic or a correct
// lowering. It never gives the text "x!" and never the internal error of the printer.
TEST(NonNullTranslation, UnusualContextsGiveADiagnosticOrCorrectCode)
{
    for (const char* body : {
             "    void A($Node a, int x = a!.v) { }\n",
             "    void A(int x = Make()!.v) { }\n",
             "    int k = Make()!.v;\n",
             "    static_assert(sizeof(int) == 4);\n    void A($Node a) { int[a!.v] arr; }\n",
             "    void A() { int[Make()!.v] arr; }\n",
             "    void A($Node a) { var f = [a!]() => 1; }\n",
             "    [attr(a!)] void A($Node a) { }\n",
             "    void A($Node a) { std.string s = \"v ${a!.v}\"; }\n",
             "    void A($Node a) { for (int i = 0; i < a!.v; i++) { } while (a!.v > 0) { } }\n",
             "    int A($Node a) { return a!.v > 0 ? a!.v : 1; }\n",
             "    void A($Node a) { switch (a!.v) { case 1: break; } }\n",
         })
    {
        const hotc::TranslateResult r = check_translate(kNode + "$Node Make() { return null; }\n", body);
        EXPECT_EQ(r.diagnostics.find("internal error"), std::string::npos) << body << r.diagnostics;
        if (r.ok)
            EXPECT_FALSE(has(r.cpp, "!.") || has(r.cpp, "a!") || has(r.cpp, ")!")) << body << r.cpp;
        else
            EXPECT_FALSE(r.diagnostics.empty()) << body;
    }
}
