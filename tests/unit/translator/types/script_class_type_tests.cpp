// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests for the type model with script classes: handle by default, [value] inline (design 7.10.1).
#include "support/type_model_test_support.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace type_model_test;

namespace
{

ast::ClassDef script_class(const std::string& name, const std::string& base, bool is_value = false)
{
    ast::ClassDef c;
    c.name = name;
    c.base = base;
    c.is_value = is_value;
    return c;
}

std::vector<ast::ClassDef> one(const std::string& name, const std::string& base, bool is_value = false)
{
    std::vector<ast::ClassDef> v;
    v.push_back(script_class(name, base, is_value));
    return v;
}

} // namespace

TEST(ScriptClassType, ShellClassUsesTheDefaultHandle)
{
    Fixture f;
    f.model.add_script_classes(one("Squad", "Unit"));
    EXPECT_EQ(f.model.lookup("Squad").kind, Kind::Handle);
    EXPECT_EQ(f.model.storage("Squad"), "\x01std::shared_ptr<Squad>");
    EXPECT_FALSE(f.model.is_script_value("Squad"));
}

TEST(ScriptClassType, ShellClassUsesTheHandleOfTheNativeBase)
{
    Fixture f;
    f.add({stub("Object", "std::unique_ptr<$T>")});
    f.model.add_script_classes(one("Item", "Object"));
    EXPECT_EQ(f.model.lookup("Item").kind, Kind::Handle);
    EXPECT_EQ(f.model.storage("Item"), "std::unique_ptr<Item>");
}

TEST(ScriptClassType, PlainClassIsAHandle)
{
    Fixture f;
    f.model.add_script_classes(one("Counter", ""));
    EXPECT_EQ(f.model.storage("Counter"), "\x01std::shared_ptr<Counter>");
}

TEST(ScriptClassType, ValueClassIsStoredInline)
{
    Fixture f;
    f.model.add_script_classes(one("Stats", "", true));
    EXPECT_TRUE(f.model.is_script_value("Stats"));
    EXPECT_EQ(f.model.lookup("Stats").kind, Kind::Value);
    EXPECT_EQ(f.model.storage("Stats"), "Stats");
}

TEST(ScriptClassType, OtherNamesAreNotScriptClasses)
{
    Fixture f;
    f.model.add_script_classes(one("Stats", "", true));
    EXPECT_FALSE(f.model.is_script_value("int"));
    EXPECT_FALSE(f.model.is_script_value("Other"));
}
