// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests for the type model: the stub "class *;", "default_handle" and the rules of design 7.10.3.
#include "support/type_model_test_support.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace type_model_test;

namespace
{

ast::Stub star(const std::string& default_handle = kShared)
{
    ast::Stub s = stub("*");
    s.default_handle = default_handle;
    return s;
}

// A model whose headers have: Mesh (a class), Vec3 (plain data), Weapon (a class).
void with_classes(Fixture& f)
{
    f.model.set_classes([](const std::string& n)
    {
        TypeModel::ClassFacts c;
        c.known = n == "Mesh" || n == "Vec3" || n == "Weapon" || n == "std::string";
        c.plain = n == "Vec3";
        return c;
    });
}

} // namespace

TEST(DefaultHandle, FallbackGivesTheDefaultHandleToAClass)
{
    Fixture f;
    f.add({star()});
    with_classes(f);
    ASSERT_TRUE(f.diags.items.empty());
    EXPECT_EQ(f.model.storage("Mesh"), "std::shared_ptr<Mesh>");
    EXPECT_EQ(f.model.param("Mesh"), "const std::shared_ptr<Mesh>&");
    EXPECT_EQ(f.model.make("Mesh", "1"), "std::make_shared<Mesh>(1)");
    EXPECT_EQ(f.model.lookup("Mesh").owner, "*");
}

TEST(DefaultHandle, PlainDataIsAValueAndNeverGetsTheFallback)
{
    Fixture f;
    f.add({star()});
    with_classes(f);
    EXPECT_EQ(f.model.lookup("Vec3").kind, Kind::Value);
    EXPECT_EQ(f.model.storage("Vec3"), "Vec3");
}

TEST(DefaultHandle, BuiltinAndUnknownTypesStayRaw)
{
    Fixture f;
    f.add({star()});
    with_classes(f);
    EXPECT_EQ(f.model.lookup("int").kind, Kind::Raw);
    EXPECT_EQ(f.model.lookup("Unknown").kind, Kind::Raw);
    EXPECT_EQ(f.model.storage("Mesh*"), "Mesh*");
    EXPECT_EQ(f.model.storage("Unknown"), "Unknown");
}

TEST(DefaultHandle, ExactStubAndPatternBeatTheFallback)
{
    Fixture f;
    f.add({star(), stub("Weapon", "", true)});
    f.add({stub("std::vector<*>", "", true)});
    with_classes(f);
    EXPECT_EQ(f.model.lookup("Weapon").kind, Kind::Value);
    EXPECT_EQ(f.model.lookup("std::vector<int>").kind, Kind::Value);
    EXPECT_EQ(f.model.storage("Weapon"), "Weapon");
}

TEST(DefaultHandle, WithoutTheStubAClassHasNoHandle)
{
    Fixture f;
    with_classes(f);
    EXPECT_EQ(f.model.lookup("Mesh").kind, Kind::Raw);
    EXPECT_TRUE(f.model.wants_facts("Mesh")); // the facts can still make it a value
}

// Design 7.10.2 rule 4: plain data is a value with no stub and no "class *;".
TEST(DefaultHandle, PlainDataIsAValueWithoutAnyStub)
{
    Fixture f;
    with_classes(f);
    EXPECT_EQ(f.model.lookup("Vec3").kind, Kind::Value);
    EXPECT_EQ(f.model.storage("Vec3"), "Vec3");
    EXPECT_EQ(f.model.param("Vec3"), "Vec3");
}

TEST(DefaultHandle, WantsFactsOnlyForAClassNameWithoutAStub)
{
    Fixture f;
    f.add({star(), stub("Weapon", kShared)});
    EXPECT_TRUE(f.model.wants_facts("Mesh"));
    EXPECT_FALSE(f.model.wants_facts("Weapon"));
    EXPECT_FALSE(f.model.wants_facts("int"));
    EXPECT_FALSE(f.model.wants_facts("Mesh*"));
    EXPECT_FALSE(f.model.wants_facts("const Mesh"));
}

TEST(DefaultHandle, LaterListOverridesAndNeedsNoMessage)
{
    Fixture f;
    f.add({star()});
    f.add({star("Tex<$T>")});
    with_classes(f);
    EXPECT_TRUE(f.diags.items.empty());
    EXPECT_TRUE(f.diags.warnings.empty());
    EXPECT_EQ(f.model.storage("Mesh"), "Tex<Mesh>");
}

TEST(DefaultHandle, ScriptOverrideOfThePreludeIsAWarning)
{
    Fixture f;
    f.add({star()});
    f.model.add_stubs({star("Tex<$T>")}, f.diags, f.map, TypeModel::Source::Script);
    EXPECT_TRUE(f.diags.items.empty());
    ASSERT_EQ(f.diags.warnings.size(), 1u);
    EXPECT_NE(f.diags.warnings[0].msg.find("overrides"), std::string::npos);
    EXPECT_EQ(f.diags.warnings[0].severity, "warning");
}

TEST(DefaultHandle, ScriptStubThatOverridesAPreludeStubIsAWarning)
{
    Fixture f;
    f.add({stub("Weapon", kShared)});
    f.model.add_stubs({stub("Weapon", "", true), stub("Other", kShared)}, f.diags, f.map, TypeModel::Source::Script);
    EXPECT_TRUE(f.diags.items.empty());
    ASSERT_EQ(f.diags.warnings.size(), 1u);
    EXPECT_NE(f.diags.warnings[0].msg.find("'Weapon'"), std::string::npos);
    EXPECT_EQ(f.model.lookup("Weapon").kind, Kind::Value);
}

TEST(DefaultHandle, PreludeOverPreludeIsSilentAndScriptOnlyStubIsSilent)
{
    Fixture f;
    f.add({stub("Weapon", kShared)});
    f.add({stub("Weapon", "", true)});
    f.model.add_stubs({stub("Other", kShared)}, f.diags, f.map, TypeModel::Source::Script);
    EXPECT_TRUE(f.diags.items.empty());
    EXPECT_TRUE(f.diags.warnings.empty());
}

TEST(DefaultHandle, ValueOnStarIsAnError)
{
    Fixture f;
    f.add({stub("*", "", true)});
    ASSERT_EQ(f.diags.items.size(), 1u);
    EXPECT_NE(f.diags.items[0].msg.find("'value' is not allowed on 'class *'"), std::string::npos);
}

TEST(DefaultHandle, OtherAttributesOnStarAreAnError)
{
    Fixture f;
    f.add({stub("*", kShared)});
    ASSERT_EQ(f.diags.items.size(), 1u);
    EXPECT_NE(f.diags.items[0].msg.find("only 'native', 'default_handle' and 'weak_handle'"), std::string::npos);
    ast::Stub s = stub("*");
    s.intrusive_add = "A";
    s.intrusive_release = "R";
    f.add({s});
    EXPECT_EQ(f.diags.items.size(), 2u);
}

TEST(DefaultHandle, TwoStarsInOneFileAreAnError)
{
    Fixture f;
    f.add({star(), star("Tex<$T>")});
    ASSERT_EQ(f.diags.items.size(), 1u);
    EXPECT_NE(f.diags.items[0].msg.find("only one 'class *'"), std::string::npos);
}

TEST(DefaultHandle, DefaultHandleOnAnotherClassIsAnError)
{
    Fixture f;
    ast::Stub s = stub("Weapon");
    s.default_handle = kShared;
    f.add({s});
    ASSERT_EQ(f.diags.items.size(), 1u);
    EXPECT_NE(f.diags.items[0].msg.find("valid only on 'class *'"), std::string::npos);
}

TEST(DefaultHandle, ValueTogetherWithHandleIsAnError)
{
    Fixture f;
    ast::Stub s = stub("Weapon", kShared, true);
    f.add({s});
    ASSERT_EQ(f.diags.items.size(), 1u);
    EXPECT_NE(f.diags.items[0].msg.find("exclusive"), std::string::npos);
}

TEST(DefaultHandle, StarWithoutDefaultHandleClearsTheFallback)
{
    Fixture f;
    f.add({star()});
    f.add({stub("*")});
    with_classes(f);
    EXPECT_TRUE(f.diags.items.empty());
    EXPECT_EQ(f.model.lookup("Mesh").kind, Kind::Raw);
}

// "weak_handle" gives the storage of a "[weak]" declaration. It needs a handle template.
TEST(DefaultHandle, WeakHandleSetsTheWeakStorage)
{
    Fixture f;
    with_classes(f);
    ast::Stub base = stub("Weapon", "Ref<$T>");
    base.weak_handle = "WeakRef<$T>";
    ast::Stub all = star("Own<$T>");
    all.weak_handle = "WeakOwn<$T>";
    ast::Stub bad = stub("Bad", "$T*");
    bad.weak_handle = "WeakRef<$T>";
    f.add({base, all, bad});
    ASSERT_EQ(f.diags.items.size(), 1u);
    EXPECT_NE(f.diags.items[0].msg.find("'weak_handle' needs a 'handle' template"), std::string::npos);
    EXPECT_EQ(f.model.weak_storage("Weapon"), "WeakRef<Weapon>");
    EXPECT_EQ(f.model.weak_storage("Mesh"), "WeakOwn<Mesh>");
    EXPECT_EQ(f.model.custom_handle_templates(), (std::vector<std::string>{"Own", "Ref"}));
}
