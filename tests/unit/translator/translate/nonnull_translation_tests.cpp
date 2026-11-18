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
