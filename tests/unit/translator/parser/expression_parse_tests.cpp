// Tests for the parser of the expressions: operators, calls, names, literals and lists.
// Most tests are round trips: parse the text, print the tree and compare the C++ text.
// The printer writes a fixed layout, so the expected text can differ in spacing.
#include <gtest/gtest.h>

#include <string>
#include <variant>
#include <vector>

#include "emit.h"
#include "parser/expression.h"
#include "support/expression_parse_test_support.h"

using namespace sppc;
using namespace expr_test;

TEST(ExpressionParse, Precedence)
{
    EXPECT_EQ(round_trip("a + b * c"), "a + b * c");
    EXPECT_EQ(round_trip("a = b = c"), "a = b = c");
    EXPECT_EQ(round_trip("a < b == c"), "a < b == c");
    EXPECT_EQ(round_trip("-a++"), "-a++");
    EXPECT_EQ(round_trip("a ? b : c ? d : e"), "a ? b : c ? d : e");
    EXPECT_EQ(round_trip("a << 2"), "a << 2");
    EXPECT_EQ(round_trip("a < b"), "a < b");
    EXPECT_EQ(round_trip("a || b && c | d ^ e & f"), "a || b && c | d ^ e & f");
    EXPECT_EQ(round_trip("a <<= b >>= c"), "a <<= b >>= c");
    EXPECT_EQ(round_trip("a += b -= c *= d /= e %= f &= g |= h ^= i"),
              "a += b -= c *= d /= e %= f &= g |= h ^= i");
    EXPECT_EQ(round_trip("x == y != z <= w >= v"), "x == y != z <= w >= v");
}

// "is" and "as" (design 7.7) bind tighter than a comparison and looser than a shift.
TEST(ExpressionParse, IsAndAs)
{
    EXPECT_EQ(round_trip("a is B"), "a is B");
    EXPECT_EQ(round_trip("x as Game.Unit"), "x as Game::Unit");
    EXPECT_EQ(round_trip("a is B == c"), "a is B == c");
    EXPECT_EQ(round_trip("!(a is B) && c"), "!(a is B) && c");
    Parsed p("a << 1 as B");
    ASSERT_TRUE(p.expr);
    const auto* t = std::get_if<ast::TypeTestExpr>(&p.expr->v);
    ASSERT_TRUE(t);
    EXPECT_TRUE(t->is_as);
    EXPECT_EQ(t->type, "B");
}

TEST(ExpressionParse, Tree)
{
    // The tree has the right shape, not only the right text.
    Parsed p("a + b * c");
    ASSERT_TRUE(p.expr);
    const auto* top = std::get_if<ast::BinaryExpr>(&p.expr->v);
    ASSERT_TRUE(top);
    EXPECT_EQ(top->op, "+");
    EXPECT_TRUE(std::get_if<ast::BinaryExpr>(&top->rhs->v));
    Parsed q("a = b = c");
    ASSERT_TRUE(q.expr);
    const auto* asg = std::get_if<ast::BinaryExpr>(&q.expr->v);
    ASSERT_TRUE(asg);
    EXPECT_TRUE(std::get_if<ast::BinaryExpr>(&asg->rhs->v));  // right associative
    Parsed r("a - b - c");
    ASSERT_TRUE(r.expr);
    const auto* sub = std::get_if<ast::BinaryExpr>(&r.expr->v);
    ASSERT_TRUE(sub);
    EXPECT_TRUE(std::get_if<ast::BinaryExpr>(&sub->lhs->v));  // left associative
}

TEST(ExpressionParse, PrefixAndPostfix)
{
    EXPECT_EQ(round_trip("- -a"), "- -a");
    EXPECT_EQ(round_trip("!a && ~b"), "!a && ~b");
    EXPECT_EQ(round_trip("*p++"), "*p++");
    EXPECT_EQ(round_trip("a & b && c"), "a & b && c");
    EXPECT_EQ(round_trip("++i + --j"), "++i + --j");
    EXPECT_EQ(round_trip("a++ + b"), "a++ + b");
    EXPECT_EQ(round_trip("a - -b"), "a - -b");
    EXPECT_EQ(round_trip("a-->b"), "a-- > b");
}

TEST(ExpressionParse, CallsAndMembers)
{
    EXPECT_EQ(round_trip("this.Move(Vec3(2.0 * this.speed * dt, 0, 0))"),
              "this.Move(Vec3(2.0 * this.speed * dt, 0, 0))");
    // The printer joins the parts of a name with "::". The lowering pass chooses the form.
    EXPECT_EQ(round_trip("Math.Max(this.health - amount, 0.0)"),
              "Math::Max(this.health - amount, 0.0)");
    EXPECT_EQ(round_trip("std.printf(\"x\", a)"), "std::printf(\"x\", a)");
    EXPECT_EQ(round_trip("v.size() < n"), "v::size() < n");
    EXPECT_EQ(round_trip("x[i + 1].y.z"), "x[i + 1].y.z");
    EXPECT_EQ(round_trip("f().x"), "f().x");
    EXPECT_EQ(round_trip("this.p.m"), "this.p.m");
    EXPECT_EQ(round_trip("(a).x"), "(a).x");
    EXPECT_EQ(round_trip("f()"), "f()");
    EXPECT_EQ(round_trip("f(a)(b)[c]"), "f(a)(b)[c]");
    EXPECT_EQ(round_trip("base.Tick(dt)"), "base.Tick(dt)");
    EXPECT_EQ(round_trip("x = null"), "x = nullptr");
}

TEST(ExpressionParse, Names)
{
    EXPECT_EQ(round_trip("static_cast<int>(x)"), "static_cast<int32_t>(x)");
    EXPECT_EQ(round_trip("a.b.c"), "a::b::c");
    EXPECT_EQ(round_trip("std.vector<int>.size_type"), "std::vector<int32_t>::size_type");
    EXPECT_EQ(round_trip("make<Foo<int>, 3>(x)"), "make<Foo<int32_t>, 3>(x)");
    EXPECT_EQ(round_trip("std.max<float>(a, b)"), "std::max<float>(a, b)");
    EXPECT_EQ(round_trip("pick<Color.Red>(t)"), "pick<Color::Red>(t)");
    EXPECT_EQ(round_trip("std.vector<std.string>(a)"), "std::vector<std::string>(a)");
    // A "<" that does not start template arguments is less-than.
    EXPECT_EQ(round_trip("a < b && c > (d)"), "a < b && c > (d)");
    EXPECT_EQ(round_trip("a < b > c"), "a < b > c");
    // The word "of" is a name. The words "let", "if" and so on are not.
    EXPECT_EQ(round_trip("of + x"), "of + x");
    // "<int>" is not followed by "(" or ".", so "<" is less-than and the text is bad.
    EXPECT_EQ(one_error("a.b<int>"), "1:9: expected an expression");
}

TEST(ExpressionParse, NameNode)
{
    Parsed p("a.B<int>.c<float>(x)");
    ASSERT_TRUE(p.expr);
    const auto* call = std::get_if<ast::CallExpr>(&p.expr->v);
    ASSERT_TRUE(call);
    const auto* n = std::get_if<ast::NameExpr>(&call->callee->v);
    ASSERT_TRUE(n);
    ASSERT_EQ(n->parts.size(), 3u);
    EXPECT_EQ(n->parts[1], "B<int32_t>");
    EXPECT_EQ(n->parts[2], "c");
    EXPECT_EQ(n->template_args, "<float>");
}

TEST(ExpressionParse, Literals)
{
    EXPECT_EQ(round_trip(".5 + x"), ".5 + x");
    EXPECT_EQ(round_trip("1e+5 - 2.5E-3f"), "1e+5 - 2.5E-3f");
    EXPECT_EQ(round_trip("0x1e+5"), "0x1e + 5");  // no sign in a hex number
    EXPECT_EQ(round_trip("0XFFu + 10ull"), "0XFFu + 10ull");
    EXPECT_EQ(round_trip("\"a\\\"b\" + 'c' + '\\''"), "\"a\\\"b\" + 'c' + '\\''");
    EXPECT_EQ(round_trip("  a /* c */ + b // x"), "a + b");
}

TEST(ExpressionParse, List)
{
    auto items = [](const std::string& text, Diagnostics& diag) {
        const LineMap map(text);
        return parse_expression_list({text, &map}, {0, static_cast<uint32_t>(text.size())},
                                     diag);
    };
    Diagnostics d1;
    auto v = items("a, f(b, c), g(1, 2), x ? y : z", d1);
    EXPECT_EQ(d1.items.size(), 0u);
    ASSERT_EQ(v.size(), 4u);
    EXPECT_EQ(ast::print(*v[1]), "f(b, c)");
    EXPECT_EQ(ast::print(*v[2]), "g(1, 2)");
    EXPECT_EQ(ast::print(*v[3]), "x ? y : z");

    Diagnostics d2;
    EXPECT_TRUE(items("  /* none */ ", d2).empty());
    EXPECT_EQ(d2.items.size(), 0u);

    Diagnostics d3;
    EXPECT_TRUE(items("a, b,", d3).empty());
    ASSERT_EQ(d3.items.size(), 1u);
    EXPECT_EQ(d3.items[0].msg, "expected an expression");
    EXPECT_EQ(d3.items[0].col, 6);

    Diagnostics d4;
    EXPECT_TRUE(items("a b", d4).empty());
    ASSERT_EQ(d4.items.size(), 1u);
    EXPECT_EQ(d4.items[0].msg, "expected end of expression");
}

TEST(ExpressionParse, NewExpression)
{
    EXPECT_EQ(round_trip("new Weapon(1, 2)"), "new Weapon(1, 2)");
    EXPECT_EQ(round_trip("new Weapon()"), "new Weapon()");
    EXPECT_EQ(round_trip("new std.vector<float>()"), "new std::vector<float>()");
    EXPECT_EQ(round_trip("new A(new B(1), x + 1)"), "new A(new B(1), x + 1)");
    EXPECT_EQ(round_trip("new Weapon(1).dmg"), "new Weapon(1).dmg");
    Parsed p("new Weapon(1, 2)");
    ASSERT_TRUE(p.expr != nullptr);
    const auto* n = std::get_if<ast::NewExpr>(&p.expr->v);
    ASSERT_TRUE(n != nullptr);
    EXPECT_EQ(n->type, "Weapon");
    EXPECT_EQ(n->args.size(), 2u);
    EXPECT_EQ(n->pos, 0u);
}

TEST(ExpressionParse, Offsets)
{
    // A span in the middle of a larger buffer. Positions are absolute.
    const std::string text = "xx;\nvar y = a + foo(b);\nzz";
    const LineMap map(text);
    const uint32_t b = static_cast<uint32_t>(text.find("a + foo"));
    const uint32_t e = static_cast<uint32_t>(text.find(");")) + 1;
    Diagnostics diag;
    ast::ExprP x = parse_expression({text, &map}, {b, e}, diag);
    ASSERT_TRUE(x);
    EXPECT_EQ(diag.items.size(), 0u);
    EXPECT_EQ(ast::pos_of(*x), b);
    const auto* bin = std::get_if<ast::BinaryExpr>(&x->v);
    ASSERT_TRUE(bin);
    EXPECT_EQ(ast::pos_of(*bin->rhs), text.find("foo"));
    const auto* call = std::get_if<ast::CallExpr>(&bin->rhs->v);
    ASSERT_TRUE(call);
    EXPECT_EQ(ast::pos_of(*call->args[0]), text.find("b)"));

    // An error in the span has the absolute line and column.
    Diagnostics d2;
    EXPECT_TRUE(parse_expression({text, &map}, {b, b + 3}, d2) == nullptr);  // "a +"
    ASSERT_EQ(d2.items.size(), 1u);
    EXPECT_EQ(d2.items[0].line, 2);
    EXPECT_EQ(d2.items[0].col, 12);
    EXPECT_EQ(d2.items[0].msg, "expected an expression");

    // The parser reads only the span: the text after it is not seen.
    Diagnostics d3;
    ast::ExprP y = parse_expression({text, &map}, {b, b + 1}, d3);
    ASSERT_TRUE(y);
    EXPECT_EQ(ast::print(*y), "a");
    EXPECT_EQ(d3.items.size(), 0u);

    // Text after the expression in the span.
    Diagnostics d4;
    EXPECT_TRUE(parse_expression({text, &map}, {b, b + 3 + 4}, d4) != nullptr);  // "a + foo"
    Diagnostics d5;
    EXPECT_TRUE(parse_expression({text, &map}, {b - 2, b + 1}, d5) == nullptr);  // "= a"
    EXPECT_EQ(d5.items.size(), 1u);
}

TEST(ExpressionParse, CastOfABuiltinType)
{
    EXPECT_EQ(round_trip("(int)f"), "(int32_t)f");
    EXPECT_EQ(round_trip("(float)i + 1"), "(float)i + 1");
    EXPECT_EQ(round_trip("( uint )  x"), "(uint32_t)x");
    EXPECT_EQ(round_trip("(double)-x * 2"), "(double)-x * 2");
    EXPECT_EQ(round_trip("(int)f(x).y"), "(int32_t)f(x).y");
    EXPECT_EQ(round_trip("(int)(a + b)"), "(int32_t)(a + b)");
    EXPECT_EQ(round_trip("(int)(float)x"), "(int32_t)(float)x");
    EXPECT_EQ(round_trip("(byte*)p"), "(uint8_t*)p");
    EXPECT_EQ(round_trip("(const int&)i"), "(const int32_t&)i");
    Parsed p("(int)f");
    ASSERT_TRUE(p.expr);
    const auto* u = std::get_if<ast::UnaryExpr>(&p.expr->v);
    ASSERT_TRUE(u);
    EXPECT_EQ(u->op, "(int32_t)");
    EXPECT_FALSE(u->postfix);
    EXPECT_EQ(p.diag.items.size(), 0u);
}

TEST(ExpressionParse, CastOfAPointerOrReferenceType)
{
    EXPECT_EQ(round_trip("(Foo*)p"), "(Foo*)p");
    EXPECT_EQ(round_trip("(const Foo&)r"), "(const Foo&)r");
    EXPECT_EQ(round_trip("(std.string*)p"), "(std::string*)p");
    EXPECT_EQ(round_trip("(Foo **)p"), "(Foo**)p");
}

TEST(ExpressionParse, ParenthesizedTextIsNotACast)
{
    EXPECT_EQ(round_trip("(x)"), "(x)");
    EXPECT_EQ(round_trip("(a * b)"), "(a * b)");
    EXPECT_EQ(round_trip("(a) + b"), "(a) + b");
    EXPECT_EQ(round_trip("(a)-b"), "(a) - b");
    EXPECT_EQ(round_trip("(a & b) * c"), "(a & b) * c");
    EXPECT_EQ(round_trip("(i) * 2"), "(i) * 2");
    EXPECT_EQ(round_trip("(x) => { }"), "[=](auto x) {}");
    EXPECT_EQ(round_trip("(int x) => { }"), "[=](int32_t x) {}");
    // A name without a mark is not sure to be a type: it is no cast. Use static_cast.
    EXPECT_EQ(one_error("(Foo)x"), "1:6: expected end of expression");
}
