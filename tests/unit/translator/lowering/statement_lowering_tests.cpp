// Tests for the lowering of numbers, declarations, loops, switch labels and finally.
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "emit.h"
#include "lower.h"
#include "parser/expression.h"
#include "parser/parser.h"
#include "parser/statement.h"
#include "support/lowering_test_support.h"

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
    EXPECT_EQ(f.body("foreach (var x in xs) { this.n = x; }"), "for (auto x : xs) { st->n = x; }");
    EXPECT_EQ(f.body("foreach (const var y in ys) { }"), "for (const auto y : ys) {}");
}

TEST(StatementLowering, ForAndControl)
{
    Fixture f;
    EXPECT_EQ(f.body("for (var i = 0; i < this.n; i++) { if (i > 2.5) { break; } }"),
              "for (auto i = 0; i < st->n; i++) { if (i > 2.5f) { break; } }");
}

TEST(StatementLowering, CaseLabelsOnSeparateLinesKeepTheirCase)
{
    Fixture f;
    const std::string text = " switch (k) {\ncase 1:\ncase 2: default:\nx = 1;\n}";
    LineMap map(text);
    f.cx.map = &map;
    ast::BlockStmt b = parse_block({text, &map}, {0, static_cast<uint32_t>(text.size())}, f.diag);
    lower_block(b, f.cx, f.diag);
    ast::Emitter e(map, "f.spp");
    ast::emit_stmts(b.stmts, e, 1);
    e.finish();
    EXPECT_EQ(f.diag.items.size(), 0u);
    EXPECT_EQ(e.text(),
              "#line 1 \"f.spp\"\n    switch (k) {\n"
              "#line 2 \"f.spp\"\n        case 1:\n"
              "#line 3 \"f.spp\"\n        case 2: default:\n"
              "#line 4 \"f.spp\"\n        x = 1;\n"
              "#line 5 \"f.spp\"\n    }\n");
}

TEST(StatementLowering, FinallyBody)
{
    Fixture f;
    EXPECT_EQ(f.body("try { throw 1; } finally { this.n = 1; }"),
              "{ spp::Finally __fin_0([&] { st->n = 1; }); { throw 1; } }");
    // A loop or a switch fully inside the finally block can use break and continue.
    // A lambda in the finally block can use return.
    f.body("while (true) { try { } finally {\n"
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
    EXPECT_EQ(f.errors(), (std::vector<std::string>{
                              "2:3: 'return' is not allowed in a 'finally' block",
                              "2:13: 'break' is not allowed in a 'finally' block",
                              "2:29: 'continue' is not allowed in a 'finally' block",
                              "2:3: 'return' is not allowed in a 'finally' block",
                          }));
}
