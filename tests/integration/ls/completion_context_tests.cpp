// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests of classify() (M5 step 16). Each case is a text with the marker "|" for the cursor.
#include "completion_context.h"

#include <gtest/gtest.h>

#include <string>

using hot::ls::classify;
using hot::ls::CompletionContext;
using K = CompletionContext::Kind;

namespace
{

struct Case
{
    const char* text;
    K kind;
};

K run(const std::string& marked)
{
    const auto bar = marked.find('|');
    std::string text = marked.substr(0, bar) + marked.substr(bar + 1);
    return classify(text, bar).kind;
}

const Case kCases[] = {
    // Base lists
    {"class X : |", K::Base},
    {"class X : A, |", K::Base},
    {"class X : Ba|", K::Base},
    {"class X<T> : |", K::Base},
    {"struct S : |", K::Base},
    {"public class X : A, B, |", K::Base},
    {"class X : A<B, |", K::Type},
    {"class X : A { int y; }\nclass Z : |", K::Base},
    // Attributes
    {"[Refl|", K::Attribute},
    {"[|", K::Attribute},
    {"[Attr(|", K::AttributeArg},
    {"[Attr(1, |", K::AttributeArg},
    {"[A(1), Re|", K::Attribute},
    {"class X {\n  [Ser|", K::Attribute},
    {"class X { int a; [Attr(|", K::AttributeArg},
    {"[A] class X : |", K::Base},
    // using
    {"using |", K::Using},
    {"using \"|", K::Using},
    {"using \"Bench|", K::Using},
    {"using Urho3D.|", K::Using},
    {"using X = |", K::Type},
    {"using X = Li|", K::Type},
    // override
    {"class X : B { override |", K::Override},
    {"class X : B { override Ru|", K::Override},
    {"class X : B { public override |", K::Override},
    // enum
    {"enum E { |", K::EnumBody},
    {"enum E { A, |", K::EnumBody},
    {"enum E { A = 1, B|", K::EnumBody},
    {"enum E { A = |", K::Expr},
    {"enum E : |", K::Type},
    // type positions
    {"class X { void F() { var a = (Nod|", K::Type},
    {"class X { void F() { var a = b as |", K::Type},
    {"class X { void F() { var a = new |", K::Type},
    {"class X { void F() { try { } catch (|", K::Type},
    {"class X { void F() { try { } catch (Ex|", K::Type},
    {"class X { void F() { var a = $|", K::Type},
    {"class X { void F() { var a = $Nod|", K::Type},
    {"class X { void F() { if (a is |", K::Type},
    {"class X { |", K::Type},
    {"class X { pub|", K::Type},
    {"class X { public static |", K::Type},
    {"class X { int a; |", K::Type},
    {"class X { void F(|", K::Type},
    {"class X { void F(int a, |", K::Type},
    {"class X { void F(int a, Nod|", K::Type},
    {"class X { void F(ref |", K::Type},
    {"class X { List<|", K::Type},
    {"class X { Dictionary<string, |", K::Type},
    {"class X { void F() { List<|", K::Type},
    {"class X { void F() { var a = new List<|", K::Type},
    {"|", K::Type},
    {"class A { } |", K::Type},
    {"namespace N { |", K::Type},
    {"class X { void F() { for (|", K::Type},
    // expressions
    {"class X { static const int X = Rin|;", K::Expr},
    {"class X { int a = |", K::Expr},
    {"class X { void F() { |", K::Expr},
    {"class X { void F() { a = |", K::Expr},
    {"class X { void F() { Node n = Ge|", K::Expr},
    {"class X { void F() { return |", K::Expr},
    {"class X { void F() { foo(|", K::Expr},
    {"class X { void F() { foo(a, |", K::Expr},
    {"class X { void F() { if (|", K::Expr},
    {"class X { void F() { if (a < |", K::Expr},
    {"class X { void F() { for (int i = 0; i < |", K::Expr},
    {"class X { void F() { a[|", K::Expr},
    {"class X { void F() { a = b + |", K::Expr},
    {"class X { void F() { while (x) |", K::Expr},
    {"class X { void F(int a = |", K::Expr},
    {"class X { void F() { new Foo(|", K::Expr},
    {"class X { void F() { int i = 0; if (i < 3) { |", K::Expr},
    // members
    {"class X { void F() { a.|", K::Member},
    {"class X { void F() { a.Fo|", K::Member},
    {"class X { void F() { Urho3D::|", K::Member},
    {"class X { void F() { p->Na|", K::Member},
    {"class X { void F() { a.\n    |", K::Member},
    {"class X { Urho3D::|", K::Member},
    // comments, strings and chars
    {"// hello |", K::None},
    {"class X { // note |", K::None},
    {"/* a |", K::None},
    {"class X { /* a \n b |", K::None},
    {"class X { void F() { var s = \"ab|", K::None},
    {"class X { void F() { var s = \"a\\\"b|", K::None},
    {"class X { void F() { var c = 'x|", K::None},
    {"class X { void F() { var c = '\\'|", K::None},
    // code after comments, strings and chars
    {"// a\nclass X : |", K::Base},
    {"/* a */ class X : |", K::Base},
    {"class X { void F() { var s = \"{ ( [\"; |", K::Expr},
    {"class X { void F() { var c = '{'; var d = '\"'; |", K::Expr},
    {"class X { void F() { var s = \"a\\\" { \"; a.|", K::Member},
    {"// class X : \nclass Y { |", K::Type},
    {"class X { /* } */ |", K::Type},
    // names are None
    {"class |", K::None},
    {"class X |", K::None},
    {"class X { int |", K::None},
    {"class X { int fo|", K::None},
    {"class X { void F() { Node |", K::None},
    // brackets and angles
    {"class X { void F() { foo(a, b); } int |", K::None},
    {"class X { void F() { if (a) { b(); } } |", K::Type},
    {"class X { void F() { a < b; |", K::Expr},
    {"class X { void F() { x = a < b && c > d; |", K::Expr},
    {"class X { List<int> a; |", K::Type},
    {"class X { Dictionary<string, List<int>> a; |", K::Type},
    {"class X { void F(List<int> a, |", K::Type},
    // review fixes
    {"class X { void F() { x = 1; // see a.\n    |", K::Expr},
    {"class X { void F() { x = 1; /* see a. */ |", K::Expr},
    {"class X { void F() { x = a * |", K::Expr},
    {"class X { void F() { x = a & |", K::Expr},
    {"class X { void F() { x = c ? |", K::Expr},
    {"class X { int a = b * |", K::Expr},
    {"class X { int a = b & |", K::Expr},
    {"class X { void F() { y = (a * |", K::Expr},
    {"class X { void F() { y = (c ? |", K::Expr},
    {"class X { void F() { y = f(a) * |", K::Expr},
    {"class X { Foo* |", K::None},
    {"class X { Foo<int>* |", K::None},
    {"class X { void F() { foreach (Node n in |", K::Expr},
    {"class X { void F() { for (|", K::Type},
    {"class X { void F() { f = [&](No|", K::Type},
    {"class X { void F() { f = [=](int a, No|", K::Type},
    {"class X { void F() { a = b[|", K::Expr},
    // A lowercase name before "<" in a function body is less-than. This is a known trade-off.
    {"class X { void F() { vector<No|", K::Expr},
    {"class X { void F() { a /|/ b", K::Expr},
};

class ClassifyTest : public ::testing::TestWithParam<Case>
{
};

TEST_P(ClassifyTest, Kind)
{
    const Case& c = GetParam();
    EXPECT_EQ(hot::ls::kindName(run(c.text)), hot::ls::kindName(c.kind)) << c.text;
}

INSTANTIATE_TEST_SUITE_P(Cases, ClassifyTest, ::testing::ValuesIn(kCases));

TEST(Classify, OffsetPastEndIsClamped)
{
    EXPECT_EQ(classify("using ", 100).kind, K::Using);
}

TEST(Classify, ExprAndTypeFlag)
{
    auto flag = [](const std::string& marked)
    {
        const auto bar = marked.find('|');
        return classify(marked.substr(0, bar) + marked.substr(bar + 1), bar).exprAndType;
    };
    EXPECT_TRUE(flag("class X { void F() { No|"));
    EXPECT_TRUE(flag("class X { void F() { |"));
    EXPECT_TRUE(flag("class X { void F() { (fo|"));
    EXPECT_TRUE(flag("class X { void F() { for (|"));
    EXPECT_FALSE(flag("class X { void F() { a = foo(|"));
    EXPECT_FALSE(flag("class X { Fo|"));
    EXPECT_FALSE(flag("class X { void F() { a = No|"));
    EXPECT_FALSE(flag("class X { void F() { a.No|"));
}

TEST(Classify, PrefixStart)
{
    const std::string t = "class X { void F() { a.Fo";
    EXPECT_EQ(classify(t, t.size()).prefixStart, t.size() - 2);
    EXPECT_EQ(classify(t, t.size() - 2).prefixStart, t.size() - 2);
}

} // namespace
