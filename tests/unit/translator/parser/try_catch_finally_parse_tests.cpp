// Tests for the parser of throw, try, catch and finally.
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "emit.h"
#include "parser/expression.h"
#include "parser/parser.h"
#include "parser/statement.h"
#include "support/statement_parse_test_support.h"

using namespace sppc;
using namespace stmt_test;

TEST(TryCatchFinallyParse, Exceptions)
{
    EXPECT_EQ(round_trip("throw 42;"), "throw 42;");
    EXPECT_EQ(round_trip("throw;"), "throw;");
    EXPECT_EQ(round_trip("throw std.runtime_error(\"x\");"), "throw std::runtime_error(\"x\");");
    EXPECT_EQ(round_trip("try { f(); } catch (int e) { g(e); }"),
              "try { f(); } catch (int32_t e) { g(e); }");
    EXPECT_EQ(round_trip("try { f(); } catch (std.exception& e) { g(); }\n"
                         "catch (const std.string& m) { throw; } catch (...) { }"),
              "try { f(); } catch (std::exception& e) { g(); } "
              "catch (const std::string& m) { throw; } catch (...) {}");
    EXPECT_EQ(round_trip("try { try { throw 1; } catch (...) { throw; } } catch (int e) { }"),
              "try { try { throw 1; } catch (...) { throw; } } catch (int32_t e) {}");
    // The words are keywords only as whole words.
    Parsed p("tryit = 1; thrown = 2; catcher(3);");
    EXPECT_EQ(p.diag.items.size(), 0u) << p.diag.format("t");
    EXPECT_EQ(p.block.stmts.size(), 3u);
    for (const auto& s : p.block.stmts) EXPECT_TRUE(std::holds_alternative<ast::ExprStmt>(s->v));
    {
        Parsed q("try { f(); } catch (int e) { }\nthrow;");
        ASSERT_EQ(q.block.stmts.size(), 2u);
        const auto* t = std::get_if<ast::TryStmt>(&q.block.stmts[0]->v);
        ASSERT_NE(t, nullptr);
        ASSERT_EQ(t->catches.size(), 1u);
        EXPECT_EQ(t->catches[0].type, "int32_t");
        EXPECT_EQ(t->catches[0].name, "e");
        EXPECT_FALSE(t->catches[0].is_all);
        EXPECT_TRUE(std::holds_alternative<ast::ThrowStmt>(q.block.stmts[1]->v));
    }
}

TEST(TryCatchFinallyParse, Finally)
{
    // A finally block with catch clauses and without catch clauses.
    {
        Parsed p("try { f(); } catch (int e) { g(e); } finally { h(); }");
        EXPECT_EQ(p.diag.items.size(), 0u) << p.diag.format("t");
        ASSERT_EQ(p.block.stmts.size(), 1u);
        const auto* t = std::get_if<ast::TryStmt>(&p.block.stmts[0]->v);
        ASSERT_NE(t, nullptr);
        ASSERT_EQ(t->catches.size(), 1u);
        EXPECT_EQ(t->catches[0].name, "e");
        ASSERT_TRUE(t->finally_block.has_value());
        EXPECT_EQ(t->finally_block->stmts.size(), 1u);
        EXPECT_EQ(t->finally_block->pos, p.text.rfind('{'));
    }
    {
        Parsed p("try { f(); } finally { h(); i(); }\nthrow;");
        EXPECT_EQ(p.diag.items.size(), 0u) << p.diag.format("t");
        ASSERT_EQ(p.block.stmts.size(), 2u);
        const auto* t = std::get_if<ast::TryStmt>(&p.block.stmts[0]->v);
        ASSERT_NE(t, nullptr);
        EXPECT_TRUE(t->catches.empty());
        ASSERT_TRUE(t->finally_block.has_value());
        EXPECT_EQ(t->finally_block->stmts.size(), 2u);
        EXPECT_TRUE(std::holds_alternative<ast::ThrowStmt>(p.block.stmts[1]->v));
    }
    {
        // A try without a finally block has no finally_block.
        Parsed p("try { f(); } catch (...) { }");
        const auto* t = std::get_if<ast::TryStmt>(&p.block.stmts.at(0)->v);
        ASSERT_NE(t, nullptr);
        EXPECT_FALSE(t->finally_block.has_value());
    }
    EXPECT_EQ(round_trip("try { f(); } catch (int e) { } finally { g(); }"),
              "{ spp::Finally __fin_0([&] { g(); }); try { f(); } catch (int32_t e) {} }");
    EXPECT_EQ(round_trip("try { f(); } finally { }"),
              "{ spp::Finally __fin_0([&] {}); { f(); } }");
    {
        // "finally" is a reserved word, so a name cannot be "finally".
        Parsed p("try { f(); } finally;");
        EXPECT_FALSE(p.diag.items.empty());
    }
}

TEST(TryCatchFinallyParse, ExceptionErrors)
{
    {
        Parsed p("try { f(); }\nb = 1;");
        EXPECT_EQ(errors_of(p), (std::vector<std::string>{"2:1: expected 'catch' or 'finally'"}))
            << p.diag.format("t");
    }
    {
        // A type without a name is not supported.
        Parsed p("a = 1;\ntry { f(); } catch (int) { }\nc = 2;");
        EXPECT_EQ(errors_of(p), (std::vector<std::string>{"2:21: expected a name"}))
            << p.diag.format("t");
        EXPECT_EQ(p.printed(), "a = 1; c = 2;");
    }
    {
        Parsed p("try { } catch (std::exception& e) { }");
        EXPECT_EQ(errors_of(p), (std::vector<std::string>{"1:19: use '.' instead of '::'"}))
            << p.diag.format("t");
    }
    {
        Parsed p("try { } catch () { }");
        EXPECT_EQ(errors_of(p), (std::vector<std::string>{"1:16: expected a type"}))
            << p.diag.format("t");
    }
    {
        // The old form "catch (name: type)" gives a clear error.
        Parsed p("try { } catch (e: int) { }");
        EXPECT_EQ(errors_of(p), (std::vector<std::string>{
                                    "1:16: catch syntax changed; write 'catch (Type name)'"}))
            << p.diag.format("t");
    }
    {
        // A pointer type and a qualified type.
        EXPECT_EQ(round_trip("try { f(); } catch (Actor* a) { } catch (std.runtime_error e) { }"),
                  "try { f(); } catch (Actor* a) {} catch (std::runtime_error e) {}");
    }
    {
        // The statements after a broken throw stay.
        Parsed p("throw 1 +;\na = 1;");
        EXPECT_EQ(p.diag.items.size(), 1u) << p.diag.format("t");
        EXPECT_EQ(p.printed(), "a = 1;");
    }
    {
        Parsed p("try { f(); } catch (e: int { }\na = 1;");
        EXPECT_GE(p.diag.items.size(), 1u);
    }
}
