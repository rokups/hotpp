// Tests for the lowering of numbers, declarations, loops, switch labels and finally.
#include "emit.h"
#include "lower.h"
#include "parser/expression.h"
#include "parser/parser.h"
#include "parser/statement.h"
#include "support/lowering_test_support.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace sppc;
using namespace lowering_test;

TEST(StatementLowering, Numbers)
{
    Fixture f;
    EXPECT_EQ(f.expr("1.5"), "1.5f");
    EXPECT_EQ(f.expr("1.5d"), "1.5");
    EXPECT_EQ(f.expr("1.5f"), "1.5f");
    EXPECT_EQ(f.expr("2e3"), "2e3f");
    EXPECT_EQ(f.expr("3L"), "int64_t{3}");
    EXPECT_EQ(f.expr("0x10"), "0x10");
    EXPECT_EQ(f.expr("7"), "7");
    EXPECT_EQ(rewrite_number("0.5"), "0.5f");
}

TEST(StatementLowering, NullAndDeclarations)
{
    Fixture f;
    EXPECT_EQ(f.body("var p = null; const Actor* q = null; int k;"),
        "auto p = nullptr; const Actor* q = nullptr; int32_t k = int32_t();");
}

TEST(StatementLowering, ForOf)
{
    Fixture f;
    EXPECT_EQ(f.body("foreach (var x in xs) { this.n = x; }"), "for (auto x : xs) { __st->n = x; }");
    EXPECT_EQ(f.body("foreach (const var y in ys) { }"), "for (const auto y : ys) {}");
}

TEST(StatementLowering, ForAndControl)
{
    Fixture f;
    EXPECT_EQ(f.body("for (var i = 0; i < this.n; i++) { if (i > 2.5) { break; } }"),
        "for (auto i = 0; i < __st->n; i++) { if (i > 2.5f) { break; } }");
}

TEST(StatementLowering, CaseLabelsOnSeparateLinesKeepTheirCase)
{
    Fixture f;
    const std::string text = " switch (k) {\ncase 1:\ncase 2: default:\nx = 1;\n}";
    LineMap map(text);
    f.cx.map = &map;
    ast::BlockStmt b = parse_block({text, &map}, {0, static_cast<uint32_t>(text.size())}, f.diag);
    lower_block(b, f.cx, f.diag);
    ast::Emitter e(map, "f.hot");
    ast::emit_stmts(b.stmts, e, 1);
    e.finish();
    EXPECT_EQ(f.diag.items.size(), 0u);
    EXPECT_EQ(e.text(),
        "#line 1 \"f.hot\"\n    switch (k) {\n"
        "#line 2 \"f.hot\"\n        case 1:\n"
        "#line 3 \"f.hot\"\n        case 2: default:\n"
        "#line 4 \"f.hot\"\n        x = 1;\n"
        "#line 5 \"f.hot\"\n    }\n");
}

TEST(StatementLowering, FinallyBody)
{
    Fixture f;
    EXPECT_EQ(f.body("try { throw 1; } finally { this.n = 1; }"),
        "{ hot::Finally __fin_0([&] { __st->n = 1; }); { throw 1; } }");
    // A loop or a switch fully inside the finally block can use break and continue.
    // A lambda in the finally block can use return.
    f.body(
        "while (true) { try { } finally {\n"
        "  while (true) { break; continue; }\n"
        "  switch (this.n) { case 1: break; }\n"
        "  var g = () => { return 1; };\n"
        "} }");
    EXPECT_EQ(f.errors(), std::vector<std::string>{});
    // A "return" in the try body or in a catch clause is correct.
    f.body("try { return; } catch (...) { return; } finally { }");
    EXPECT_EQ(f.errors(), std::vector<std::string>{});
}

TEST(StatementLowering, ErrorJumpOutOfFinally)
{
    Fixture f;
    f.body("try { } finally {\n  return;\n}");
    f.body("while (true) { try { } catch (...) { } finally {\n  if (true) break;\n} }");
    f.body("while (true) { try { } finally {\n  switch (this.n) { case 1: continue; }\n} }");
    f.body("while (true) { try { } finally { try { } finally {\n  return 1;\n} } }");
    EXPECT_EQ(f.errors(),
        (std::vector<std::string>{
            "2:3: 'return' is not allowed in a 'finally' block",
            "2:13: 'break' is not allowed in a 'finally' block",
            "2:29: 'continue' is not allowed in a 'finally' block",
            "2:3: 'return' is not allowed in a 'finally' block",
        }));
}

// A switch with string labels becomes an if / else-if chain on a local with the value.
TEST(StatementLowering, StringSwitchBecomesAnIfChain)
{
    Fixture f;
    EXPECT_EQ(f.body("switch (s) { case \"a\": f(); break; case \"b\": case \"c\": return; default: g(); }"),
        "{ const auto& __sw_1 = s; if (__sw_1 == \"a\") { f(); } else "
        "if (__sw_1 == \"b\" || __sw_1 == \"c\") { return; } else { g(); } }");
    EXPECT_EQ(f.errors().size(), 0u);
}

TEST(StatementLowering, StringSwitchWithoutDefaultAndTheLastCaseNeedsNoJump)
{
    Fixture f;
    EXPECT_EQ(f.body("switch (s) { case \"a\": f(); break; case \"b\": g(); }"),
        "{ const auto& __sw_1 = s; if (__sw_1 == \"a\") { f(); } else if (__sw_1 == \"b\") { g(); } }");
    EXPECT_EQ(f.errors().size(), 0u);
}

// A nested break needs a C++ construct. The chain goes in a do-while that runs one time.
TEST(StatementLowering, StringSwitchWithNestedBreakUsesDoWhile)
{
    Fixture f;
    EXPECT_EQ(f.body("switch (s) { case \"a\": if (x) { break; } f(); break; }"),
        "{ const auto& __sw_1 = s; do { if (__sw_1 == \"a\") { if (x) { break; } f(); } } while (false); }");
    // A break in a loop in the case belongs to the loop: no do-while.
    EXPECT_EQ(f.body("switch (s) { case \"a\": while (x) { break; } break; }"),
        "{ const auto& __sw_1 = s; if (__sw_1 == \"a\") { while (x) { break; } } }");
    EXPECT_EQ(f.errors().size(), 0u);
}

// The name counter restarts in each body. The two switches here are in one body.
TEST(StatementLowering, StringSwitchesInOneBodyGetOwnNames)
{
    Fixture f;
    EXPECT_EQ(f.body("switch (a) { case \"x\": f(); } switch (b) { case \"y\": g(); }"),
        "{ const auto& __sw_1 = a; if (__sw_1 == \"x\") { f(); } } "
        "{ const auto& __sw_2 = b; if (__sw_2 == \"y\") { g(); } }");
}

TEST(StatementLowering, NumberSwitchStaysASwitch)
{
    Fixture f;
    EXPECT_EQ(
        f.body("switch (k) { case 1: f(); break; default: g(); }"), "switch (k) { case 1: f(); break; default: g(); }");
}

TEST(StatementLowering, CollectionLiteralWithDeclaredType)
{
    Fixture f;
    EXPECT_EQ(f.body("std.vector<int> xs = [1, 2, 3]; std.map<std.string, int> m = [\"a\": 1]; "
                     "std.vector<int> e = [];"),
        "std::vector<int32_t> xs = {1, 2, 3}; std::map<std::string, int32_t> m = {{\"a\", 1}}; "
        "std::vector<int32_t> e = {};");
    EXPECT_EQ(f.errors().size(), 0u);
}

TEST(StatementLowering, CollectionLiteralAsFieldInitializer)
{
    Fixture f;
    f.cx.typed_init = true;
    EXPECT_EQ(f.expr("[1, 2]"), "{1, 2}");
    EXPECT_EQ(f.errors().size(), 0u);
}
