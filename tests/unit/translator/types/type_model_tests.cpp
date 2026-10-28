// Tests for the type model (src/translator/types.h): stubs, patterns and "make".
#include <gtest/gtest.h>

#include <map>
#include <string>
#include <vector>

#include "types.h"
#include "support/type_model_test_support.h"

using namespace sppc;
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
    EXPECT_EQ(f.model.param("Vec3"), "Vec3");
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

TEST(TypeModel, UnsupportedAttributes)
{
    Fixture f;
    ast::Stub d = stub("*");
    d.default_handle = kShared;
    f.add({d});
    ASSERT_EQ(f.diags.items.size(), 1u);
    EXPECT_NE(f.diags.items[0].msg.find("not supported yet"), std::string::npos);

    f.add({stub("*")});
    EXPECT_EQ(f.diags.items.size(), 2u);
}
