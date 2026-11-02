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

// A script class as a template argument uses the handle storage (task 48).
TEST(ScriptClassTemplateArg, VectorOfScriptClassHoldsHandles)
{
    Fixture f;
    f.model.add_script_classes(one("Child", ""));
    const std::string h = "\x01std::shared_ptr<Child>";
    EXPECT_EQ(f.model.storage("std::vector<Child>"), "std::vector<" + h + ">");
    EXPECT_EQ(f.model.storage("std::vector<Child>"), f.model.targ_storage("std::vector<Child>"));
    EXPECT_EQ(f.model.storage("std.vector<Child>"), "std.vector<" + h + ">");
}

TEST(ScriptClassTemplateArg, NestedAndMultipleArguments)
{
    Fixture f;
    f.model.add_script_classes(one("Child", ""));
    const std::string h = "\x01std::shared_ptr<Child>";
    EXPECT_EQ(f.model.storage("std::map<int, std::vector<Child>>"), "std::map<int, std::vector<" + h + ">>");
    EXPECT_EQ(f.model.storage("std::map<Child, Child>"), "std::map<" + h + ", " + h + ">");
    EXPECT_EQ(f.model.storage("std::vector<std::vector<Child>>"), "std::vector<std::vector<" + h + ">>");
}

TEST(ScriptClassTemplateArg, ParametersKeepTheirReferenceForm)
{
    Fixture f;
    f.model.add_script_classes(one("Child", ""));
    const std::string v = "std::vector<\x01std::shared_ptr<Child>>";
    EXPECT_EQ(f.model.param("std::vector<Child>"), v);
    EXPECT_EQ(f.model.param("const std::vector<Child>&"), "const " + v + "&");
    EXPECT_EQ(f.model.body_param("std::vector<Child>", false, false, false), v);
    EXPECT_EQ(f.model.body_param("std::vector<Child>", true, false, false), v);
    EXPECT_EQ(f.model.body_param("const std::vector<Child>&", false, false, false), "const " + v + "&");
}

TEST(ScriptClassTemplateArg, OtherArgumentsStayAsTheyAre)
{
    Fixture f;
    f.model.add_script_classes(one("Child", ""));
    f.model.add_script_classes(one("Stats", "", true));
    EXPECT_EQ(f.model.storage("std::vector<int>"), "std::vector<int>");
    EXPECT_EQ(f.model.storage("std::vector<Stats>"), "std::vector<Stats>"); // a [value] class
    EXPECT_EQ(f.model.storage("std::vector<Other>"), "std::vector<Other>"); // not a script class
    EXPECT_EQ(f.model.storage("std::vector<Child*>"), "std::vector<Child*>"); // a pointer
    EXPECT_EQ(f.model.storage("std::vector<ns::Child>"), "std::vector<ns::Child>");
    // A smart pointer of the class is the handle type already.
    EXPECT_EQ(f.model.storage("std::shared_ptr<Child>"), "std::shared_ptr<Child>");
    EXPECT_EQ(f.model.storage("std::vector<std::shared_ptr<Child>>"), "std::vector<std::shared_ptr<Child>>");
    // The rewrite does not apply twice.
    const std::string once = f.model.storage("std::vector<Child>");
    EXPECT_EQ(f.model.storage(once), once);
}

TEST(ScriptClassTemplateArg, ClassWithANativeBaseUsesTheHandleOfTheBase)
{
    Fixture f;
    f.add({stub("Object", "std::unique_ptr<$T>")});
    f.model.add_script_classes(one("Item", "Object"));
    EXPECT_EQ(f.model.storage("std::vector<Item>"), "std::vector<std::unique_ptr<Item>>");
}

TEST(ScriptClassTemplateArg, CppScriptKeepsItsTypes)
{
    Fixture f;
    f.model.add_script_classes(one("Child", ""));
    f.model.set_raw_script_refs(true);
    EXPECT_EQ(f.model.storage("std::vector<Child>"), "std::vector<Child>");
}

// A script class inside a function type gets the handle too: the return type and each parameter.
TEST(ScriptClassTemplateArg, FunctionTypeArgumentsAreRewritten)
{
    Fixture f;
    f.model.add_script_classes(one("Child", ""));
    const std::string h = "\x01std::shared_ptr<Child>";
    EXPECT_EQ(f.model.storage("std::function<void(Child)>"), "std::function<void(" + h + ")>");
    EXPECT_EQ(f.model.storage("std::function<Child()>"), "std::function<" + h + "()>");
    EXPECT_EQ(f.model.storage("std::function<Child(int, Child, const Child&)>"),
        "std::function<" + h + "(int, " + h + ", const " + h + "&)>");
    EXPECT_EQ(f.model.storage("std::vector<std::function<void(std::vector<Child>)>>"),
        "std::vector<std::function<void(std::vector<" + h + ">)>>");
    EXPECT_EQ(f.model.storage("std::function<void(int)>"), "std::function<void(int)>");
}
