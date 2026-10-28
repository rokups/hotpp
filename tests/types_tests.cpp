// Tests for the type model (src/translator/types.h).
#include <gtest/gtest.h>

#include <map>
#include <string>
#include <vector>

#include "types.h"

namespace {

using namespace spc;

ast::Stub stub(const std::string& name, const std::string& handle = "", bool is_value = false)
{
    ast::Stub s;
    s.name = name;
    s.is_native = true;
    s.is_value = is_value;
    s.handle = handle;
    return s;
}

struct Fixture {
    TypeModel model;
    Diagnostics diags;
    LineMap map{""};
    void add(const std::vector<ast::Stub>& stubs) { model.add_stubs(stubs, diags, map); }
};

const char* kShared = "std::shared_ptr<$T>";

TEST(Types, HandleStorageAndParam)
{
    Fixture f;
    f.add({stub("Weapon", kShared)});
    EXPECT_EQ(f.model.storage("Weapon"), "std::shared_ptr<Weapon>");
    EXPECT_EQ(f.model.param("Weapon"), "const std::shared_ptr<Weapon>&");
    EXPECT_TRUE(f.diags.items.empty());
}

TEST(Types, UnchangedTypes)
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

TEST(Types, ValuePattern)
{
    Fixture f;
    f.add({stub("std::vector<*>", "", true)});
    EXPECT_EQ(f.model.lookup("std::vector<float>").kind, Kind::Value);
    EXPECT_EQ(f.model.storage("std::vector<float>"), "std::vector<float>");
}

TEST(Types, NamespacePattern)
{
    Fixture f;
    f.add({stub("physics::*", kShared)});
    EXPECT_EQ(f.model.storage("physics::Body"), "std::shared_ptr<physics::Body>");
    EXPECT_EQ(f.model.storage("render::Body"), "render::Body");
}

TEST(Types, ExactBeatsPattern)
{
    Fixture f;
    f.add({stub("physics::*", kShared), stub("physics::Body", "", true)});
    EXPECT_EQ(f.model.lookup("physics::Body").kind, Kind::Value);
    EXPECT_EQ(f.model.storage("physics::Body"), "physics::Body");
    EXPECT_EQ(f.model.lookup("physics::Joint").kind, Kind::Handle);
}

TEST(Types, LongestPatternWins)
{
    Fixture f;
    f.add({stub("physics::*", kShared), stub("physics::detail::*", "Ptr<$T>")});
    EXPECT_EQ(f.model.storage("physics::detail::X"), "Ptr<physics::detail::X>");
    EXPECT_EQ(f.model.storage("physics::Y"), "std::shared_ptr<physics::Y>");
}

TEST(Types, LaterListOverrides)
{
    Fixture f;
    f.add({stub("Weapon", kShared)});
    f.add({stub("Weapon", "", true)});
    EXPECT_TRUE(f.diags.items.empty());
    EXPECT_EQ(f.model.storage("Weapon"), "Weapon");
}

TEST(Types, Make)
{
    Fixture f;
    f.add({stub("Weapon", kShared), stub("Tex", "TexPtr<$T>")});
    EXPECT_EQ(f.model.make("Weapon", "1, 2"), "std::make_shared<Weapon>(1, 2)");
    EXPECT_EQ(f.model.make("Vec3", ""), "Vec3()");
    EXPECT_EQ(f.model.make("Tex", "a"), "TexPtr<Tex>(new Tex(a))");
}

// Builds a base callback from a table of class name to direct bases.
TypeModel::BaseFn bases_of(std::map<std::string, std::vector<std::string>> table)
{
    return [table](const std::string& name) {
        auto it = table.find(name);
        return it == table.end() ? std::vector<std::string>{} : it->second;
    };
}

TEST(Types, BaseStubGivesHandleToSubclass)
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

TEST(Types, BaseStubWithCustomTemplateUsesSubclassName)
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

TEST(Types, ExactStubOnSubclassOverridesBase)
{
    Fixture f;
    f.add({stub("Object", kShared), stub("Item", "Ptr<$T>")});
    f.model.set_bases(bases_of({{"Item", {"Object"}}}));
    EXPECT_EQ(f.model.storage("Item"), "Ptr<Item>");
    EXPECT_EQ(f.model.lookup("Item").owner, "Item");
}

TEST(Types, NearestBaseWins)
{
    Fixture f;
    f.add({stub("Object", "Far<$T>"), stub("Mid", "Near<$T>")});
    f.model.set_bases(bases_of({{"Gadget", {"Mid"}}, {"Mid", {"Object"}}}));
    EXPECT_EQ(f.model.storage("Gadget"), "Near<Gadget>");
    EXPECT_EQ(f.model.lookup("Gadget").owner, "Mid");
}

TEST(Types, NearestBaseWinsOverEarlierDeeperBase)
{
    Fixture f;
    f.add({stub("Deep", "Far<$T>"), stub("B2", "Near<$T>")});
    f.model.set_bases(bases_of({{"Item", {"B1", "B2"}}, {"B1", {"Deep"}}}));
    EXPECT_EQ(f.model.storage("Item"), "Near<Item>");
}

TEST(Types, FirstDeclaredBaseWinsAtSameDepth)
{
    Fixture f;
    f.add({stub("A", "PA<$T>"), stub("B", "PB<$T>")});
    f.model.set_bases(bases_of({{"Item", {"A", "B"}}}));
    EXPECT_EQ(f.model.storage("Item"), "PA<Item>");
}

TEST(Types, TwoLevelChain)
{
    Fixture f;
    f.add({stub("Object", kShared)});
    f.model.set_bases(bases_of({{"Gadget", {"Item"}}, {"Item", {"Object"}}}));
    EXPECT_EQ(f.model.storage("Gadget"), "std::shared_ptr<Gadget>");
    EXPECT_EQ(f.model.storage("Item"), "std::shared_ptr<Item>");
    EXPECT_EQ(f.model.lookup("Gadget").owner, "Object");
}

TEST(Types, ValueOnBaseIsNotInherited)
{
    Fixture f;
    f.add({stub("Object", "", true)});
    f.model.set_bases(bases_of({{"Item", {"Object"}}}));
    EXPECT_EQ(f.model.storage("Item"), "Item");
    EXPECT_EQ(f.model.lookup("Item").kind, Kind::Raw);
}

TEST(Types, BaseStubBeatsPattern)
{
    Fixture f;
    f.add({stub("game::*", "Pat<$T>"), stub("game::Object", kShared)});
    f.model.set_bases(bases_of({{"game::Item", {"game::Object"}}}));
    EXPECT_EQ(f.model.storage("game::Item"), "std::shared_ptr<game::Item>");
    EXPECT_EQ(f.model.storage("game::Other"), "Pat<game::Other>");
}

TEST(Types, NoCallbackKeepsOldResult)
{
    Fixture f;
    f.add({stub("Object", kShared), stub("game::*", "Pat<$T>")});
    EXPECT_EQ(f.model.storage("Item"), "Item");
    EXPECT_EQ(f.model.storage("game::Item"), "Pat<game::Item>");
    EXPECT_TRUE(f.model.lookup("Item").owner.empty());
}

TEST(Types, BaseCycleEnds)
{
    Fixture f;
    f.add({stub("Other", kShared)});
    f.model.set_bases(bases_of({{"A", {"B"}}, {"B", {"A", "C"}}, {"C", {"A"}}}));
    EXPECT_EQ(f.model.storage("A"), "A");
}

TEST(Types, DuplicateStubIsOneError)
{
    Fixture f;
    f.add({stub("Weapon", kShared), stub("Weapon", "", true)});
    EXPECT_EQ(f.diags.items.size(), 1u);
}

TEST(Types, UnsupportedAttributes)
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

ast::Stub intrusive_stub(const std::string& name, const std::string& handle = "")
{
    ast::Stub s = stub(name, handle);
    s.intrusive_add = "AddRef";
    s.intrusive_release = "Release";
    return s;
}

TEST(Types, IntrusiveExact)
{
    Fixture f;
    f.add({intrusive_stub("Object", "$T*")});
    const std::string st = "script::Intrusive<Object, &Object::AddRef, &Object::Release>";
    EXPECT_EQ(f.model.storage("Object"), st);
    EXPECT_EQ(f.model.param("Object"), "const " + st + "&");
    EXPECT_EQ(f.model.make("Object", "1, 2"), st + "(new Object(1, 2))");
    TypeModel::Callee c = f.model.make_callee("Object");
    EXPECT_EQ(c.text, st);
    EXPECT_TRUE(c.wrap_new);
    TypeRule r = f.model.lookup("Object");
    EXPECT_EQ(r.kind, Kind::Handle);
    EXPECT_EQ(r.intrusive_add, "AddRef");
    EXPECT_EQ(r.intrusive_release, "Release");
    EXPECT_TRUE(f.diags.items.empty());
}

TEST(Types, IntrusiveWithoutHandle)
{
    Fixture f;
    f.add({intrusive_stub("Object")});
    EXPECT_EQ(f.model.storage("Object"), "script::Intrusive<Object, &Object::AddRef, &Object::Release>");
    EXPECT_TRUE(f.diags.items.empty());
}

TEST(Types, IntrusiveInherited)
{
    Fixture f;
    f.add({intrusive_stub("Object")});
    f.model.set_bases(bases_of({{"Item", {"Object"}}}));
    const std::string st = "script::Intrusive<Item, &Object::AddRef, &Object::Release>";
    EXPECT_EQ(f.model.storage("Item"), st);
    EXPECT_EQ(f.model.make("Item", ""), st + "(new Item())");
    EXPECT_EQ(f.model.lookup("Item").intrusive_add, "AddRef");
}

TEST(Types, IntrusiveErrors)
{
    Fixture f;
    ast::Stub v = intrusive_stub("A");
    v.is_value = true;
    f.add({v});
    ASSERT_EQ(f.diags.items.size(), 1u);
    EXPECT_NE(f.diags.items[0].msg.find("'value' and 'intrusive' are exclusive"), std::string::npos);

    f.add({intrusive_stub("B", "std::shared_ptr<$T>")});
    ASSERT_EQ(f.diags.items.size(), 2u);
    EXPECT_NE(f.diags.items[1].msg.find("'intrusive' needs handle(\"$T*\")"), std::string::npos);
    EXPECT_EQ(f.model.storage("B"), "B");
}

}  // namespace
