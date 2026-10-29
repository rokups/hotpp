// Tests for the lowering of names: locals, parameters and script functions, and their scope.
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

TEST(NameResolutionLowering, CatchNameIsALocal)
{
    Fixture f;
    EXPECT_EQ(f.body("try { throw 1; } catch (std.exception& e) { e.what(); } catch (...) { throw; }"),
              "try { throw 1; } catch (std::exception& e) { spp::deref(e).what(); } "
              "catch (...) { throw; }");
    EXPECT_EQ(f.body("throw this.health;"), "throw __st->health;");
    EXPECT_EQ(f.diag.items.size(), 0u);
}

TEST(NameResolutionLowering, LocalShadowsAScope)
{
    Fixture f;
    EXPECT_EQ(f.body("var std = 1; std.x;"), "auto std = 1; spp::deref(std).x;");
    EXPECT_EQ(f.diag.items.size(), 0u);
}

TEST(NameResolutionLowering, ScriptFnCall)
{
    Fixture f;
    EXPECT_EQ(f.expr("this.heal(1.0)"), "Player__heal(__self, 1.0f)");
    EXPECT_EQ(f.expr("this.heal()"), "Player__heal(__self)");
    EXPECT_EQ(f.expr("heal(1.0)"), "Player__heal(__self, 1.0f)");
    EXPECT_EQ(f.expr("heal()"), "Player__heal(__self)");
    EXPECT_EQ(f.diag.items.size(), 0u);
}

TEST(NameResolutionLowering, LocalHidesScriptFn)
{
    Fixture f;
    f.param("heal");
    EXPECT_EQ(f.expr("heal(1)"), "heal(1)");
}

TEST(NameResolutionLowering, LocalDoesNotLeakIntoEarlierText)
{
    Fixture f;
    // The use is before the declaration, so the name is not yet a local.
    EXPECT_EQ(f.body("Foo.bar(); var Foo = 1;"), "Foo::bar(); auto Foo = 1;");
    EXPECT_EQ(f.diag.items.size(), 0u);
}

TEST(NameResolutionLowering, LocalLeaksOutOfABlock)
{
    Fixture f;
    // The old rewriter has no block scope: Foo stays a local after the block.
    EXPECT_EQ(f.body("{ var Foo = 1; } Foo.bar();"), "{ auto Foo = 1; } spp::deref(Foo).bar();");
}

TEST(NameResolutionLowering, ScriptFnHiddenAfterBlock)
{
    Fixture f;
    EXPECT_EQ(f.body("{ var heal = 1; } heal(2);"), "{ auto heal = 1; } heal(2);");
}

TEST(NameResolutionLowering, LambdaParameterLeaks)
{
    Fixture f;
    EXPECT_EQ(f.body("var g = (int* q) => { }; q.x;"),
              "auto g = spp::make_closure(GEN, \"Player::run lambda\", [=](int32_t* q) "
              "{ SPP_NODEBUG_LOCAL auto* __st = state_of(__self); }); spp::deref(q).x;");
}

TEST(NameResolutionLowering, ForOfVariableIsLocal)
{
    Fixture f;
    EXPECT_EQ(f.body("foreach (var Foo in xs) { Foo.a(); }"), "for (auto Foo : xs) { spp::deref(Foo).a(); }");
}

TEST(NameResolutionLowering, ErrorScriptFnMustBeCalled)
{
    Fixture f;
    EXPECT_EQ(f.body("var a = 1;\nvar b = this.heal;"), "auto a = 1; auto b = Player__heal;");
    ASSERT_EQ(f.errors().size(), 1u);
    EXPECT_EQ(f.errors()[0], "2:9: script function 'heal' must be called");
}
