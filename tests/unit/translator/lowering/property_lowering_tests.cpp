// Tests for the lowering of the properties: reads, writes and the accessors of other objects.
#include "emit.h"
#include "lower.h"
#include "parser/expression.h"
#include "parser/parser.h"
#include "parser/statement.h"
#include "support/lowering_test_support.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace hotc;
using namespace lowering_test;

TEST(PropertyLowering, PropertyReadAndWrite)
{
    Fixture f;
    add_props(f);
    EXPECT_EQ(f.expr("this.Hp"), "Player__get_Hp(__self)");
    EXPECT_EQ(f.expr("Hp + Max"), "Player__get_Hp(__self) + Player__get_Max(__self)");
    EXPECT_EQ(f.body("Hp = 1;"), "Player__set_Hp(__self, 1);");
    EXPECT_EQ(f.body("this.Hp = this.Max;"), "Player__set_Hp(__self, Player__get_Max(__self));");
    EXPECT_EQ(f.body("this.Hp *= 2 + 1;"), "Player__set_Hp(__self, Player__get_Hp(__self) * (2 + 1));");
    EXPECT_EQ(f.body("Hp <<= 1;"), "Player__set_Hp(__self, Player__get_Hp(__self) << (1));");
    EXPECT_EQ(f.body("--Hp;"), "Player__set_Hp(__self, Player__get_Hp(__self) - 1);");
    EXPECT_EQ(f.body("this.Hp++;"), "Player__set_Hp(__self, Player__get_Hp(__self) + 1);");
    EXPECT_EQ(f.body("Sink = Hp == 1 ? 2 : 3;"), "Player__set_Sink(__self, Player__get_Hp(__self) == 1 ? 2 : 3);");
    // A comparison is not a write.
    EXPECT_EQ(f.expr("Hp <= 2"), "Player__get_Hp(__self) <= 2");
    EXPECT_EQ(f.diag.items.size(), 0u) << f.errors()[0];
}

// "obj.P" on another object of the class. The object goes through hot::shell_of.
TEST(PropertyLowering, PropertyOfOtherObjectRead)
{
    Fixture f;
    add_props(f);
    f.param("p");
    const std::string sh = "hot::shell_of<Player_Shell>(p, \"Player\")";
    EXPECT_EQ(f.expr("p.Hp"), "Player__get_Hp(" + sh + ")");
    EXPECT_EQ(f.expr("p.Hp + p.Max"), "Player__get_Hp(" + sh + ") + Player__get_Max(" + sh + ")");
    EXPECT_EQ(f.expr("this.target.Hp"), "Player__get_Hp(hot::shell_of<Player_Shell>(__st->target, \"Player\"))");
    EXPECT_EQ(f.expr("p.target.Hp"), "Player__get_Hp(hot::shell_of<Player_Shell>(hot::deref(p).target, \"Player\"))");
    EXPECT_EQ(f.expr("f(p).Hp"), "Player__get_Hp(hot::shell_of<Player_Shell>(f(p), \"Player\"))");
    EXPECT_EQ(f.diag.items.size(), 0u) << f.errors()[0];
}

TEST(PropertyLowering, PropertyOfOtherObjectWrite)
{
    Fixture f;
    add_props(f);
    f.param("p");
    const std::string sh = "hot::shell_of<Player_Shell>(p, \"Player\")";
    EXPECT_EQ(f.body("p.Hp = 1;"), "Player__set_Hp(" + sh + ", 1);");
    EXPECT_EQ(f.body("p.Sink = p.Hp;"), "Player__set_Sink(" + sh + ", Player__get_Hp(" + sh + "));");
    EXPECT_EQ(
        f.body("this.target.Hp = 2;"), "Player__set_Hp(hot::shell_of<Player_Shell>(__st->target, \"Player\"), 2);");
    EXPECT_EQ(f.diag.items.size(), 0u) << f.errors()[0];
}

// A compound write on another object evaluates the object once: hot::prop_update.
TEST(PropertyLowering, PropertyOfOtherObjectCompoundWrite)
{
    Fixture f;
    add_props(f);
    f.param("p");
    const std::string sh = "hot::shell_of<Player_Shell>(p, \"Player\")";
    EXPECT_EQ(f.body("p.Hp += 2;"), "hot::prop_update<hot::op_add>(Player__get_Hp, Player__set_Hp, " + sh + ", 2);");
    EXPECT_EQ(
        f.body("p.Hp <<= 1 + 1;"), "hot::prop_update<hot::op_shl>(Player__get_Hp, Player__set_Hp, " + sh + ", 1 + 1);");
    EXPECT_EQ(f.diag.items.size(), 0u) << f.errors()[0];
}

TEST(PropertyLowering, PropertyOfOtherObjectIncrement)
{
    Fixture f;
    add_props(f);
    f.param("p");
    const std::string sh = "hot::shell_of<Player_Shell>(p, \"Player\")";
    const std::string acc = "Player__get_Hp, Player__set_Hp, " + sh + ", 1)";
    EXPECT_EQ(f.body("p.Hp++;"), "hot::prop_update<hot::op_add>(" + acc + ";");
    EXPECT_EQ(f.body("--p.Hp;"), "hot::prop_update<hot::op_sub>(" + acc + ";");
    // The value of "P++" is the old value. The value of "++P" is the new value.
    EXPECT_EQ(f.expr("p.Hp++"), "hot::prop_post<hot::op_add>(" + acc);
    EXPECT_EQ(f.expr("++p.Hp"), "hot::prop_update<hot::op_add>(" + acc);
    EXPECT_EQ(f.expr("Hp--"), "hot::prop_post<hot::op_sub>(Player__get_Hp, Player__set_Hp, __self, 1)");
    EXPECT_EQ(f.diag.items.size(), 0u) << f.errors()[0];
}

// A write in an expression gives the assigned value. A write in a statement stays a call.
TEST(PropertyLowering, PropertyWriteAsExpression)
{
    Fixture f;
    add_props(f);
    f.param("p");
    f.param("a");
    const std::string sh = "hot::shell_of<Player_Shell>(p, \"Player\")";
    EXPECT_EQ(f.body("a = this.Hp = 1;"), "a = hot::prop_set(Player__set_Hp, __self, 1);");
    EXPECT_EQ(f.body("a = (p.Hp = 2);"), "a = (hot::prop_set(Player__set_Hp, " + sh + ", 2));");
    EXPECT_EQ(f.body("Hp = Sink = 3;"), "Player__set_Hp(__self, hot::prop_set(Player__set_Sink, __self, 3));");
    EXPECT_EQ(f.body("a = Hp *= 2;"), "a = hot::prop_update<hot::op_mul>(Player__get_Hp, Player__set_Hp, __self, 2);");
    EXPECT_EQ(f.diag.items.size(), 0u) << f.errors()[0];
}

// A member that is not a property keeps the member access. "base.P" is not a property.
TEST(PropertyLowering, PropertyNameOnlyRewritesProperties)
{
    Fixture f;
    add_props(f);
    f.param("p");
    EXPECT_EQ(f.expr("p.health"), "hot::deref(p).health");
    EXPECT_EQ(f.body("p.health = 1;"), "hot::deref(p).health = 1;");
    EXPECT_EQ(f.body("p.health += 1;"), "hot::deref(p).health += 1;");
    EXPECT_EQ(f.body("p.health++;"), "hot::deref(p).health++;");
    EXPECT_EQ(f.expr("base.Hp"), "__self->Actor::Hp");
    EXPECT_EQ(f.diag.items.size(), 0u) << f.errors()[0];
}

TEST(PropertyLowering, PropertyOfOtherObjectMissingAccessor)
{
    Fixture f;
    add_props(f);
    f.param("p");
    f.body("p.Max = 1;");
    f.body("p.Max += 1;");
    f.body("p.Max++;");
    f.expr("p.Sink");
    EXPECT_EQ(f.errors(),
        (std::vector<std::string>{
            "1:1: property 'Max' has no 'set' accessor",
            "1:1: property 'Max' has no 'set' accessor",
            "1:1: property 'Max' has no 'set' accessor",
            "1:1: property 'Sink' has no 'get' accessor",
        }));
}

TEST(PropertyLowering, PropertyLocalHidesProperty)
{
    Fixture f;
    add_props(f);
    f.param("Hp");
    EXPECT_EQ(f.body("Hp = 1;"), "Hp = 1;");
    EXPECT_EQ(f.body("this.Hp = 1;"), "Player__set_Hp(__self, 1);");
    EXPECT_EQ(f.diag.items.size(), 0u);
}

TEST(PropertyLowering, PropertyMissingAccessor)
{
    Fixture f;
    add_props(f);
    f.body("Max = 1;");
    f.body("this.Max += 1;");
    f.body("Max++;");
    f.expr("this.Sink + 1");
    EXPECT_EQ(f.errors(),
        (std::vector<std::string>{
            "1:1: property 'Max' has no 'set' accessor",
            "1:1: property 'Max' has no 'set' accessor",
            "1:1: property 'Max' has no 'set' accessor",
            "1:1: property 'Sink' has no 'get' accessor",
        }));
}

TEST(PropertyLowering, PropertyInFieldInitializerIsAnError)
{
    Fixture f;
    add_props(f);
    f.cx.allow_this = false;
    f.expr("Hp + 1");
    ASSERT_EQ(f.errors().size(), 1u);
    EXPECT_EQ(f.errors()[0], "1:1: property 'Hp' is not allowed in a field initializer");
}
