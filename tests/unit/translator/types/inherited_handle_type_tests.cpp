// Tests for the type model: a handle stub on a base class gives a handle to the subclasses.
#include <gtest/gtest.h>

#include <map>
#include <string>
#include <vector>

#include "types.h"
#include "support/type_model_test_support.h"

using namespace sppc;
using namespace type_model_test;

TEST(InheritedHandleType, BaseStubGivesHandleToSubclass)
{
    Fixture f;
    f.add({stub("Object", kShared)});
    f.model.set_bases(bases_of({{"Item", {"Object"}}}));
    EXPECT_EQ(f.model.storage("Item"), "std::shared_ptr<Item>");
    EXPECT_EQ(f.model.param("Item"), "const std::shared_ptr<Item>&");
    EXPECT_EQ(f.model.make("Item", "a"), "std::make_shared<Item>(a)");
    EXPECT_EQ(f.model.make_callee("Item").text, "std::make_shared<Item>");
    EXPECT_EQ(f.model.lookup("Item").owner, "Object");
    EXPECT_EQ(f.model.lookup("Object").owner, "Object");
}

TEST(InheritedHandleType, BaseStubWithCustomTemplateUsesSubclassName)
{
    Fixture f;
    f.add({stub("Object", "TexPtr<$T>")});
    f.model.set_bases(bases_of({{"Item", {"Object"}}}));
    EXPECT_EQ(f.model.storage("Item"), "TexPtr<Item>");
    EXPECT_EQ(f.model.make("Item", "a"), "TexPtr<Item>(new Item(a))");
    auto c = f.model.make_callee("Item");
    EXPECT_EQ(c.text, "TexPtr<Item>");
    EXPECT_TRUE(c.wrap_new);
}

TEST(InheritedHandleType, ExactStubOnSubclassOverridesBase)
{
    Fixture f;
    f.add({stub("Object", kShared), stub("Item", "Ptr<$T>")});
    f.model.set_bases(bases_of({{"Item", {"Object"}}}));
    EXPECT_EQ(f.model.storage("Item"), "Ptr<Item>");
    EXPECT_EQ(f.model.lookup("Item").owner, "Item");
}

TEST(InheritedHandleType, NearestBaseWins)
{
    Fixture f;
    f.add({stub("Object", "Far<$T>"), stub("Mid", "Near<$T>")});
    f.model.set_bases(bases_of({{"Gadget", {"Mid"}}, {"Mid", {"Object"}}}));
    EXPECT_EQ(f.model.storage("Gadget"), "Near<Gadget>");
    EXPECT_EQ(f.model.lookup("Gadget").owner, "Mid");
}

TEST(InheritedHandleType, NearestBaseWinsOverEarlierDeeperBase)
{
    Fixture f;
    f.add({stub("Deep", "Far<$T>"), stub("B2", "Near<$T>")});
    f.model.set_bases(bases_of({{"Item", {"B1", "B2"}}, {"B1", {"Deep"}}}));
    EXPECT_EQ(f.model.storage("Item"), "Near<Item>");
}

TEST(InheritedHandleType, FirstDeclaredBaseWinsAtSameDepth)
{
    Fixture f;
    f.add({stub("A", "PA<$T>"), stub("B", "PB<$T>")});
    f.model.set_bases(bases_of({{"Item", {"A", "B"}}}));
    EXPECT_EQ(f.model.storage("Item"), "PA<Item>");
}

TEST(InheritedHandleType, TwoLevelChain)
{
    Fixture f;
    f.add({stub("Object", kShared)});
    f.model.set_bases(bases_of({{"Gadget", {"Item"}}, {"Item", {"Object"}}}));
    EXPECT_EQ(f.model.storage("Gadget"), "std::shared_ptr<Gadget>");
    EXPECT_EQ(f.model.storage("Item"), "std::shared_ptr<Item>");
    EXPECT_EQ(f.model.lookup("Gadget").owner, "Object");
}

TEST(InheritedHandleType, ValueOnBaseIsNotInherited)
{
    Fixture f;
    f.add({stub("Object", "", true)});
    f.model.set_bases(bases_of({{"Item", {"Object"}}}));
    EXPECT_EQ(f.model.storage("Item"), "Item");
    EXPECT_EQ(f.model.lookup("Item").kind, Kind::Raw);
}

TEST(InheritedHandleType, BaseStubBeatsPattern)
{
    Fixture f;
    f.add({stub("game::*", "Pat<$T>"), stub("game::Object", kShared)});
    f.model.set_bases(bases_of({{"game::Item", {"game::Object"}}}));
    EXPECT_EQ(f.model.storage("game::Item"), "std::shared_ptr<game::Item>");
    EXPECT_EQ(f.model.storage("game::Other"), "Pat<game::Other>");
}

TEST(InheritedHandleType, NoCallbackKeepsOldResult)
{
    Fixture f;
    f.add({stub("Object", kShared), stub("game::*", "Pat<$T>")});
    EXPECT_EQ(f.model.storage("Item"), "Item");
    EXPECT_EQ(f.model.storage("game::Item"), "Pat<game::Item>");
    EXPECT_TRUE(f.model.lookup("Item").owner.empty());
}

TEST(InheritedHandleType, BaseCycleEnds)
{
    Fixture f;
    f.add({stub("Other", kShared)});
    f.model.set_bases(bases_of({{"A", {"B"}}, {"B", {"A", "C"}}, {"C", {"A"}}}));
    EXPECT_EQ(f.model.storage("A"), "A");
}
