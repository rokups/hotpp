// Tests for the parser of the class stubs: [native], handle, value, intrusive and patterns.
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "parser/parser.h"
#include "support/script_parse_test_support.h"

using namespace spc;
using namespace script_test;

TEST(NativeStubParse, NativeValueString)
{
    StubParsed p("[native, value] class std.string;\n");
    ASSERT_EQ(p.diag.items.size(), 0u) << p.diag.format("t");
    ASSERT_EQ(p.script.stubs.size(), 1u);
    EXPECT_TRUE(p.script.classes.empty());
    EXPECT_EQ(p.script.stubs[0].name, "std::string");
    EXPECT_TRUE(p.script.stubs[0].is_native);
    EXPECT_TRUE(p.script.stubs[0].is_value);
}

TEST(NativeStubParse, Handle)
{
    StubParsed p("[native, handle(\"std::shared_ptr<$T>\")] class Weapon;\n");
    ASSERT_EQ(p.diag.items.size(), 0u) << p.diag.format("t");
    ASSERT_EQ(p.script.stubs.size(), 1u);
    EXPECT_EQ(p.script.stubs[0].name, "Weapon");
    EXPECT_EQ(p.script.stubs[0].handle, "std::shared_ptr<$T>");
    EXPECT_FALSE(p.script.stubs[0].is_value);
}

TEST(NativeStubParse, TemplatePattern)
{
    StubParsed p("[native, value] class std.vector<*>;\n[native, value] class physics.*;\n");
    ASSERT_EQ(p.diag.items.size(), 0u) << p.diag.format("t");
    ASSERT_EQ(p.script.stubs.size(), 2u);
    EXPECT_EQ(p.script.stubs[0].name, "std::vector<*>");
    EXPECT_EQ(p.script.stubs[1].name, "physics::*");
}

TEST(NativeStubParse, Intrusive)
{
    StubParsed p("[native, handle(\"$T*\"), intrusive(\"AddRef\", \"Release\")] class Object;\n");
    ASSERT_EQ(p.diag.items.size(), 0u) << p.diag.format("t");
    ASSERT_EQ(p.script.stubs.size(), 1u);
    EXPECT_EQ(p.script.stubs[0].handle, "$T*");
    EXPECT_EQ(p.script.stubs[0].intrusive_add, "AddRef");
    EXPECT_EQ(p.script.stubs[0].intrusive_release, "Release");
}

TEST(NativeStubParse, DefaultHandle)
{
    StubParsed p("[native, default_handle(\"std::shared_ptr<$T>\")] class *;\n");
    ASSERT_EQ(p.diag.items.size(), 0u) << p.diag.format("t");
    ASSERT_EQ(p.script.stubs.size(), 1u);
    EXPECT_EQ(p.script.stubs[0].name, "*");
    EXPECT_EQ(p.script.stubs[0].default_handle, "std::shared_ptr<$T>");
}

TEST(NativeStubParse, ValueScriptClass)
{
    StubParsed p("[value] class Stats : Base { }\nclass Other : Base { }\n");
    ASSERT_EQ(p.diag.items.size(), 0u) << p.diag.format("t");
    ASSERT_EQ(p.script.classes.size(), 2u);
    EXPECT_TRUE(p.script.classes[0].is_value);
    EXPECT_FALSE(p.script.classes[1].is_value);
    EXPECT_TRUE(p.script.stubs.empty());
}

TEST(NativeStubParse, NativeOnClassBodyIsError)
{
    StubParsed p("[native] class Foo : Base { }\n");
    ASSERT_EQ(p.diag.items.size(), 1u) << p.diag.format("t");
    EXPECT_EQ(p.diag.items[0].msg, "attribute 'native' is not allowed on a script class");
    EXPECT_EQ(p.diag.items[0].pos, 1u);
}

TEST(NativeStubParse, AttributeErrors)
{
    {
        StubParsed p("[native, handle] class A;\n");
        ASSERT_EQ(p.diag.items.size(), 1u) << p.diag.format("t");
        EXPECT_EQ(p.diag.items[0].msg, "attribute 'handle' takes 1 arguments");
    }
    {
        StubParsed p("[native, native] class A;\n");
        ASSERT_EQ(p.diag.items.size(), 1u) << p.diag.format("t");
        EXPECT_EQ(p.diag.items[0].msg, "duplicate attribute 'native'");
    }
    {
        StubParsed p("[native, bogus] class A;\n");
        ASSERT_EQ(p.diag.items.size(), 1u) << p.diag.format("t");
        EXPECT_EQ(p.diag.items[0].msg, "unknown attribute 'bogus'");
    }
    {
        StubParsed p("class A;\n");
        ASSERT_EQ(p.diag.items.size(), 1u) << p.diag.format("t");
        EXPECT_EQ(p.diag.items[0].msg, "a stub needs the attribute 'native'");
    }
    {
        StubParsed p("[native, value, handle(\"x\")] class A;\n");
        ASSERT_EQ(p.diag.items.size(), 1u) << p.diag.format("t");
        EXPECT_EQ(p.diag.items[0].msg, "'value' and 'handle' are exclusive");
    }
    {
        StubParsed p("[native, handle(x)] class A;\n");
        EXPECT_GE(p.diag.items.size(), 1u);
        EXPECT_EQ(p.diag.items[0].msg, "expected a string in [alias(...)]");
    }
}

// A non-string attribute argument on a stub gives exactly one error.
TEST(NativeStubParse, NonStringAttributeArgumentGivesOneError)
{
    for (const char* text : {"[native, handle(x)] class A;\n", "[native, handle(1)] class A;\n",
                             "[native, intrusive(\"AddRef\", x)] class A;\n"}) {
        StubParsed p(text);
        ASSERT_EQ(p.diag.items.size(), 1u) << text << p.diag.format("t");
        EXPECT_EQ(p.diag.items[0].msg, "expected a string in [alias(...)]");
    }
}
