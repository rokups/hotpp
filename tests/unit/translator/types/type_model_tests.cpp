// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests for the type model (src/translator/types.h): stubs, patterns and "make".
#include "support/type_model_test_support.h"
#include "types.h"

#include <gtest/gtest.h>

#include <map>
#include <string>
#include <vector>

using namespace hotc;
using namespace type_model_test;

TEST(TypeModel, HandleStorageAndParam)
{
    Fixture f;
    f.add({stub("Weapon", kShared)});
    EXPECT_EQ(f.model.storage("Weapon"), "std::shared_ptr<Weapon>");
    EXPECT_EQ(f.model.param("Weapon"), "const std::shared_ptr<Weapon>&");
    EXPECT_TRUE(f.diags.items.empty());
}

TEST(TypeModel, UnchangedTypes)
{
    Fixture f;
    f.add({stub("Weapon", kShared)});
    EXPECT_EQ(f.model.storage("Weapon*"), "Weapon*");
    EXPECT_EQ(f.model.storage("Weapon&"), "Weapon&");
    EXPECT_EQ(f.model.storage("const Weapon"), "const Weapon");
    EXPECT_EQ(f.model.storage("float"), "float");
    EXPECT_EQ(f.model.storage("Vec3"), "Vec3");
    EXPECT_EQ(f.model.param("Vec3"), "hot::In<Vec3>");
    EXPECT_EQ(f.model.param("int32_t"), "int32_t");
    EXPECT_EQ(f.model.param("float*"), "float*");
}

TEST(TypeModel, ValuePattern)
{
    Fixture f;
    f.add({stub("std::vector<*>", "", true)});
    EXPECT_EQ(f.model.lookup("std::vector<float>").kind, Kind::Value);
    EXPECT_EQ(f.model.storage("std::vector<float>"), "std::vector<float>");
}

TEST(TypeModel, NamespacePattern)
{
    Fixture f;
    f.add({stub("physics::*", kShared)});
    EXPECT_EQ(f.model.storage("physics::Body"), "std::shared_ptr<physics::Body>");
    EXPECT_EQ(f.model.storage("render::Body"), "render::Body");
}

TEST(TypeModel, ExactBeatsPattern)
{
    Fixture f;
    f.add({stub("physics::*", kShared), stub("physics::Body", "", true)});
    EXPECT_EQ(f.model.lookup("physics::Body").kind, Kind::Value);
    EXPECT_EQ(f.model.storage("physics::Body"), "physics::Body");
    EXPECT_EQ(f.model.lookup("physics::Joint").kind, Kind::Handle);
}

TEST(TypeModel, LongestPatternWins)
{
    Fixture f;
    f.add({stub("physics::*", kShared), stub("physics::detail::*", "Ptr<$T>")});
    EXPECT_EQ(f.model.storage("physics::detail::X"), "Ptr<physics::detail::X>");
    EXPECT_EQ(f.model.storage("physics::Y"), "std::shared_ptr<physics::Y>");
}

TEST(TypeModel, LaterListOverrides)
{
    Fixture f;
    f.add({stub("Weapon", kShared)});
    f.add({stub("Weapon", "", true)});
    EXPECT_TRUE(f.diags.items.empty());
    EXPECT_EQ(f.model.storage("Weapon"), "Weapon");
}

TEST(TypeModel, Make)
{
    Fixture f;
    f.add({stub("Weapon", kShared), stub("Tex", "TexPtr<$T>")});
    EXPECT_EQ(f.model.make("Weapon", "1, 2"), "std::make_shared<Weapon>(1, 2)");
    EXPECT_EQ(f.model.make("Vec3", ""), "Vec3()");
    EXPECT_EQ(f.model.make("Tex", "a"), "TexPtr<Tex>(new Tex(a))");
}

TEST(TypeModel, DuplicateStubIsOneError)
{
    Fixture f;
    f.add({stub("Weapon", kShared), stub("Weapon", "", true)});
    EXPECT_EQ(f.diags.items.size(), 1u);
}

// ---- cpp_storage: the one mapper from a script type text to a C++ type text.
namespace
{

// A model with a handle stub "Weapon", a value stub "Vec3", a script class "Squad" and a struct "Pt".
struct StorageFixture
{
    Fixture f;
    StorageFixture()
    {
        f.add({stub("Weapon", kShared), stub("Vec3", "", true)});
        std::vector<ast::ClassDef> classes(2);
        classes[0].name = "Squad";
        classes[1].name = "Pt";
        classes[1].is_value = true;
        f.model.add_script_classes(classes);
    }
    std::string map(const std::string& t, TypeModel::StorageUse use = TypeModel::StorageUse::Local) const
    {
        return f.model.cpp_storage(t, use);
    }
    std::string error(const std::string& t) const
    {
        std::string e;
        EXPECT_TRUE(f.model.cpp_storage(t, TypeModel::StorageUse::Local, &e).empty());
        return e;
    }
};

} // namespace

TEST(CppStorage, HandleMarkGivesTheHandleOfTheRule)
{
    StorageFixture s;
    EXPECT_EQ(s.map("$Weapon"), "std::shared_ptr<Weapon>");
    EXPECT_EQ(s.map("$Squad"), "\x01std::shared_ptr<Squad>");
    EXPECT_EQ(s.map("$Weapon", TypeModel::StorageUse::Param), "const std::shared_ptr<Weapon>&");
}

TEST(CppStorage, BareScriptClassIsTheBox)
{
    StorageFixture s;
    EXPECT_EQ(s.map("Squad"), "hot::Value<Squad>");
    EXPECT_EQ(s.map("Weapon"), "std::shared_ptr<Weapon>");
}

TEST(CppStorage, HandleMarkOnAStructGivesASharedPointer)
{
    StorageFixture s;
    EXPECT_EQ(s.map("$Pt"), "\x01std::shared_ptr<Pt>");
    EXPECT_EQ(s.map("$Vec3"), "\x01std::shared_ptr<Vec3>");
}

TEST(CppStorage, PointerAndBuiltinStayAsTheyAre)
{
    StorageFixture s;
    EXPECT_EQ(s.map("Weapon*"), "Weapon*");
    EXPECT_EQ(s.map("const Weapon*"), "const Weapon*");
    EXPECT_EQ(s.map("int32_t"), "int32_t");
    EXPECT_EQ(s.map("float"), "float");
    EXPECT_EQ(s.map("std::array<int32_t, 3>"), "std::array<int32_t, 3>");
}

TEST(CppStorage, OptionalOfAValueType)
{
    StorageFixture s;
    EXPECT_EQ(s.map("int32_t?"), "std::optional<int32_t>");
    EXPECT_EQ(s.map("Vec3?"), "std::optional<Vec3>");
    EXPECT_EQ(s.map("Pt?"), "std::optional<Pt>");
    EXPECT_EQ(s.map("std::string?"), "std::optional<std::string>");
    EXPECT_EQ(s.map("int32_t?", TypeModel::StorageUse::Param), "hot::In<std::optional<int32_t>>");
    EXPECT_EQ(s.map("const int32_t?&"), "const std::optional<int32_t>&");
    EXPECT_EQ(s.map("int32_t?&"), "std::optional<int32_t>&");
}

TEST(CppStorage, OptionalNeedsAValueType)
{
    StorageFixture s;
    const std::string message = "\"?\" needs a value type";
    EXPECT_EQ(s.error("Weapon?"), message);
    EXPECT_EQ(s.error("Squad?"), message);
    EXPECT_EQ(s.error("Weapon*?"), message);
    EXPECT_EQ(s.error("$Weapon?"), message);
    EXPECT_EQ(s.error("std::vector<Weapon?>"), message);
}

TEST(CppStorage, SpanOfAnyElement)
{
    StorageFixture s;
    EXPECT_EQ(s.map("int32_t[]"), "std::span<int32_t>");
    EXPECT_EQ(s.map("const int32_t[]"), "std::span<const int32_t>");
    EXPECT_EQ(s.map("int32_t[]", TypeModel::StorageUse::Param), "std::span<int32_t>");
    EXPECT_EQ(s.map("Weapon[]"), "std::span<std::shared_ptr<Weapon>>");
    EXPECT_EQ(s.map("int32_t?[]"), "std::span<std::optional<int32_t>>");
}

TEST(CppStorage, MarksInTemplateArguments)
{
    StorageFixture s;
    EXPECT_EQ(s.map("std::vector<$Squad>"), "std::vector<\x01std::shared_ptr<Squad>>");
    EXPECT_EQ(s.map("std::vector<Squad>"), "std::vector<hot::Value<Squad>>");
    EXPECT_EQ(s.map("std::vector<int32_t?>"), "std::vector<std::optional<int32_t>>");
    EXPECT_EQ(s.map("std::map<int32_t, $Weapon>"), "std::map<int32_t, std::shared_ptr<Weapon>>");
}

TEST(CppStorage, UseSelectsTheForm)
{
    StorageFixture s;
    EXPECT_EQ(s.map("Weapon", TypeModel::StorageUse::Field), "std::shared_ptr<Weapon>");
    EXPECT_EQ(s.map("Weapon", TypeModel::StorageUse::Return), "std::shared_ptr<Weapon>");
    EXPECT_EQ(s.map("Weapon", TypeModel::StorageUse::Param), "const std::shared_ptr<Weapon>&");
    EXPECT_EQ(s.map("Vec3", TypeModel::StorageUse::Param), "hot::In<Vec3>");
    EXPECT_EQ(s.map("Vec3", TypeModel::StorageUse::LambdaParam), "Vec3");
}
