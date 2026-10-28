// Tests for the parser of the expressions (src/translator/parser/expression.h).
// Most tests are round trips: parse the text, print the tree and compare the C++ text.
// The printer writes a fixed layout, so the expected text can differ in spacing.
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "emit.h"
#include "parser/expression.h"

namespace {

using namespace spc;

// A parse of the whole text. The map and the text live as long as the object.
struct Parsed {
    std::string text;
    LineMap map;
    Diagnostics diag;
    ast::ExprP expr;

    explicit Parsed(std::string t) : text(std::move(t)), map(text)
    {
        expr = parse_expression({text, &map}, {0, static_cast<uint32_t>(text.size())}, diag);
    }
};

std::string round_trip(const std::string& text)
{
    Parsed p(text);
    EXPECT_EQ(p.diag.items.size(), 0u) << text << "\n" << p.diag.format("t");
    EXPECT_TRUE(p.expr != nullptr) << text;
    return p.expr ? ast::print(*p.expr) : "<null>";
}

// The text of the one error: "line:col: text".
std::string one_error(const std::string& text)
{
    Parsed p(text);
    EXPECT_TRUE(p.expr == nullptr) << text;
    EXPECT_EQ(p.diag.items.size(), 1u) << text << "\n" << p.diag.format("t");
    if (p.diag.items.empty()) return "<none>";
    const Diag& d = p.diag.items[0];
    return std::to_string(d.line) + ":" + std::to_string(d.col) + ": " + d.msg;
}

TEST(ExprParse, Precedence)
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

TEST(ExprParse, Tree)
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

TEST(ExprParse, PrefixAndPostfix)
{
    EXPECT_EQ(round_trip("- -a"), "- -a");
    EXPECT_EQ(round_trip("!a && ~b"), "!a && ~b");
    EXPECT_EQ(round_trip("*p++"), "*p++");
    EXPECT_EQ(round_trip("&a[1]"), "&a[1]");
    EXPECT_EQ(round_trip("++i + --j"), "++i + --j");
    EXPECT_EQ(round_trip("a++ + b"), "a++ + b");
    EXPECT_EQ(round_trip("a - -b"), "a - -b");
    EXPECT_EQ(round_trip("a-->b"), "a-- > b");
}

TEST(ExprParse, CallsAndMembers)
{
    EXPECT_EQ(round_trip("this.Move({2.0 * this.speed * dt, 0, 0})"),
              "this.Move({2.0 * this.speed * dt, 0, 0})");
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
    EXPECT_EQ(round_trip("super.Tick(dt)"), "super.Tick(dt)");
    EXPECT_EQ(round_trip("x = null"), "x = nullptr");
}

TEST(ExprParse, Names)
{
    EXPECT_EQ(round_trip("static_cast<int>(x)"), "static_cast<int>(x)");
    EXPECT_EQ(round_trip("a.b.c"), "a::b::c");
    EXPECT_EQ(round_trip("std.vector<int>.size_type"), "std::vector<int>::size_type");
    EXPECT_EQ(round_trip("make<Foo<int>, 3>(x)"), "make<Foo<int>, 3>(x)");
    EXPECT_EQ(round_trip("std.max<float>(a, b)"), "std::max<float>(a, b)");
    EXPECT_EQ(round_trip("get<Color.Red>(t)"), "get<Color::Red>(t)");
    EXPECT_EQ(round_trip("std.vector<std.string>{a}"), "std::vector<std::string>{a}");
    // A "<" that does not start template arguments is less-than.
    EXPECT_EQ(round_trip("a < b && c > (d)"), "a < b && c > (d)");
    EXPECT_EQ(round_trip("a < b > c"), "a < b > c");
    // The word "of" is a name. The words "let", "if" and so on are not.
    EXPECT_EQ(round_trip("of + x"), "of + x");
    // "<int>" is not followed by "(", "{" or ".", so "<" is less-than and the text is bad.
    EXPECT_EQ(one_error("a.b<int>"), "1:9: expected an expression");
}

TEST(ExprParse, NameNode)
{
    Parsed p("a.B<int>.c<float>(x)");
    ASSERT_TRUE(p.expr);
    const auto* call = std::get_if<ast::CallExpr>(&p.expr->v);
    ASSERT_TRUE(call);
    const auto* n = std::get_if<ast::NameExpr>(&call->callee->v);
    ASSERT_TRUE(n);
    ASSERT_EQ(n->parts.size(), 3u);
    EXPECT_EQ(n->parts[1], "B<int>");
    EXPECT_EQ(n->parts[2], "c");
    EXPECT_EQ(n->template_args, "<float>");
}

TEST(ExprParse, Bracedlists)
{
    EXPECT_EQ(round_trip("Vec3{1, 2, 3}"), "Vec3{1, 2, 3}");
    EXPECT_EQ(round_trip("{1, {2, 3}}"), "{1, {2, 3}}");
    EXPECT_EQ(round_trip("{}"), "{}");
    EXPECT_EQ(round_trip("std.array<int, 2>{1, 2}"), "std::array<int, 2>{1, 2}");
    Parsed p("Vec3{1}");
    ASSERT_TRUE(p.expr);
    EXPECT_TRUE(std::get_if<ast::TypedBracedExpr>(&p.expr->v));
}

TEST(ExprParse, Literals)
{
    EXPECT_EQ(round_trip(".5 + x"), ".5 + x");
    EXPECT_EQ(round_trip("1e+5 - 2.5E-3f"), "1e+5 - 2.5E-3f");
    EXPECT_EQ(round_trip("0x1e+5"), "0x1e + 5");  // no sign in a hex number
    EXPECT_EQ(round_trip("0XFFu + 10ull"), "0XFFu + 10ull");
    EXPECT_EQ(round_trip("\"a\\\"b\" + 'c' + '\\''"), "\"a\\\"b\" + 'c' + '\\''");
    EXPECT_EQ(round_trip("  a /* c */ + b // x"), "a + b");
}

TEST(ExprParse, Lambda)
{
    Parsed p("(x: float, q: int*) => { }");
    ASSERT_TRUE(p.expr);
    EXPECT_EQ(ast::print(*p.expr), "[=](float x, int* q) {}");
    EXPECT_EQ(p.diag.items.size(), 0u);
    const auto* l = std::get_if<ast::LambdaExpr>(&p.expr->v);
    ASSERT_TRUE(l);
    EXPECT_EQ(l->body.pos, 23u);  // the "{"
    EXPECT_EQ(l->body.end_pos, 25u);  // the "}"

    Parsed e("() => { x; }");
    ASSERT_TRUE(e.expr);
    EXPECT_EQ(ast::print(*e.expr), "[=]() { x; }");

    // A lambda as an argument; and the paren form still works.
    Parsed a("f((n: int) => { }, (a + b) * c)");
    ASSERT_TRUE(a.expr);
    EXPECT_EQ(ast::print(*a.expr), "f([=](int n) {}, (a + b) * c)");
    EXPECT_EQ(round_trip("(a.b)"), "(a::b)");
}

TEST(ExprParse, Errors)
{
    EXPECT_EQ(one_error("a +"), "1:4: expected an expression");
    EXPECT_EQ(one_error("(a"), "1:3: expected ')'");
    EXPECT_EQ(one_error("f(a,"), "1:5: expected an expression");
    EXPECT_EQ(one_error("a b"), "1:3: expected end of expression");
    EXPECT_EQ(one_error("f(a b)"), "1:5: expected ')'");
    EXPECT_EQ(one_error("a[1"), "1:4: expected ']'");
    EXPECT_EQ(one_error("{1, 2"), "1:6: expected '}'");
    EXPECT_EQ(one_error("if"), "1:1: expected an expression");
    EXPECT_EQ(one_error("a +\n  )"), "2:3: expected an expression");
    EXPECT_EQ(one_error(""), "1:1: expected an expression");
    EXPECT_EQ(one_error("\"abc"), "1:5: unterminated string or character literal");
}

// The script has no "::" and no "->". Each one gives an error at the token.
TEST(ExprParse, ScopeAndArrowErrors)
{
    EXPECT_EQ(one_error("std::printf(1)"), "1:4: use '.' instead of '::'");
    EXPECT_EQ(one_error("a + b::c"), "1:6: use '.' instead of '::'");
    EXPECT_EQ(one_error("::a"), "1:1: use '.' instead of '::'");
    EXPECT_EQ(one_error("x = ::std.max(1)"), "1:5: use '.' instead of '::'");
    EXPECT_EQ(one_error("p->m"), "1:2: use '.' instead of '->'");
    EXPECT_EQ(one_error("f(a, x.y->z)"), "1:9: use '.' instead of '->'");
    EXPECT_EQ(one_error("this->m"), "1:5: use '.' instead of '->'");
    // The "::" in template arguments: the expression parses, the diagnostics have the error.
    Parsed t("std.vector<std::string>{}");
    ASSERT_EQ(t.diag.items.size(), 1u) << t.diag.format("t");
    EXPECT_EQ(t.diag.items[0].msg, "use '.' instead of '::'");
    EXPECT_EQ(t.diag.items[0].col, 15u);
    Parsed u("f<a::B>(x)");
    ASSERT_EQ(u.diag.items.size(), 1u) << u.diag.format("t");
    EXPECT_EQ(u.diag.items[0].msg, "use '.' instead of '::'");
    EXPECT_EQ(u.diag.items[0].col, 4u);
}

TEST(ExprParse, Offsets)
{
    // A span in the middle of a larger buffer. Positions are absolute.
    const std::string text = "xx;\nlet y = a + foo(b);\nzz";
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

TEST(ExprParse, List)
{
    auto items = [](const std::string& text, Diagnostics& diag) {
        const LineMap map(text);
        return parse_expression_list({text, &map}, {0, static_cast<uint32_t>(text.size())},
                                     diag);
    };
    Diagnostics d1;
    auto v = items("a, f(b, c), {1, 2}, x ? y : z", d1);
    EXPECT_EQ(d1.items.size(), 0u);
    ASSERT_EQ(v.size(), 4u);
    EXPECT_EQ(ast::print(*v[1]), "f(b, c)");
    EXPECT_EQ(ast::print(*v[2]), "{1, 2}");
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

TEST(ExprParse, Truncation)
{
    // Each prefix of a long expression ends: no crash and no hang.
    const std::string full =
        "x = this.Move({2.0 * this.speed * dt, 0, 0}) + Math.Max(a ? b : c, x[i + 1].y->z) "
        "<< 2 > Vec3{1, 2, 3} && static_cast<int>(y) < n || (x: float, q: int*) => { f(); } "
        "|| std::vector<int>::size_type(s) + 'c' + \"str\" + 0x1e+5 + .5e-3f ;";
    size_t ok = 0;
    for (size_t n = 0; n <= full.size(); ++n) {
        const std::string text = full.substr(0, n);
        const LineMap map(text);
        Diagnostics diag;
        ast::ExprP x = parse_expression({text, &map}, {0, static_cast<uint32_t>(n)}, diag);
        if (x) ++ok;
        if (!x) EXPECT_GE(diag.items.size(), 1u) << text;
    }
    EXPECT_GT(ok, 0u);
}

TEST(ExprParse, DeepNesting)
{
    auto fails_with = [](const std::string& text, const std::string& msg) {
        Parsed p(text);
        EXPECT_TRUE(p.expr == nullptr);
        ASSERT_GE(p.diag.items.size(), 1u);
        // A generic error can have the text ", found ..." after the message.
        EXPECT_EQ(p.diag.items[0].msg.rfind(msg, 0), 0u) << p.diag.items[0].msg;
    };
    const size_t n = 100000;
    fails_with(std::string(n, '('), "maximum recursion depth exceeded");
    fails_with(std::string(n, '(') + "a" + std::string(n, ')'),
               "maximum recursion depth exceeded");
    fails_with(std::string(n, '['), "expected an expression");
    {
        std::string t;
        for (size_t i = 0; i < n; ++i) t += "a[";
        fails_with(t, "maximum recursion depth exceeded");
    }
    fails_with("f(" + std::string(n, '{'), "maximum recursion depth exceeded");
    fails_with(std::string(n, '-') + "a", "maximum operator nesting level exceeded");
    fails_with(std::string(n, '!') + "a", "maximum operator nesting level exceeded");
    {
        std::string t;
        for (size_t i = 0; i < n; ++i) t += "a = ";
        fails_with(t + "a", "maximum operator nesting level exceeded");
    }
    {
        std::string t;
        for (size_t i = 0; i < n; ++i) t += "a ? ";
        fails_with(t, "maximum recursion depth exceeded");
    }
    // A moderate depth works.
    std::string ok = std::string(100, '(') + "a" + std::string(100, ')');
    EXPECT_EQ(round_trip(ok), ok);
}

TEST(ExprParse, LongInput)
{
    // A long chain of a left associative operator, within the limit of the operators.
    std::string t = "a";
    for (int i = 0; i < 200; ++i) t += " + b";
    Parsed p(t);
    EXPECT_TRUE(p.expr != nullptr);
    // A long argument list is not a nesting.
    std::string args = "f(";
    for (int i = 0; i < 5000; ++i) args += (i ? ", x" : "x");
    args += ")";
    Parsed q(args);
    EXPECT_TRUE(q.expr != nullptr) << q.diag.format("t");
    EXPECT_EQ(q.diag.items.size(), 0u);
}

}  // namespace
