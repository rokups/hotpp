// Tests for the handle carriers and the match of a script override to a native virtual.
#include "support/type_model_test_support.h"
#include "types.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace hotc;
using namespace type_model_test;

namespace
{

struct CarrierFixture : Fixture
{
    CarrierFixture()
    {
        ast::Stub item = stub("Item", "$T*");
        item.intrusive_add = "AddRef";
        item.intrusive_release = "Release";
        add({stub("Weapon", kShared), item, stub("Plain")});
    }
};

VirtualSig sig(const std::string& name, std::vector<std::string> ptypes, bool is_const = false)
{
    return {name, std::move(ptypes), is_const};
}

} // namespace

TEST(CarrierOf, SharedPtrHandle)
{
    CarrierFixture f;
    EXPECT_EQ(f.model.carrier_of("Weapon*"), "Weapon");
    EXPECT_EQ(f.model.carrier_of("const Weapon*"), "Weapon");
    EXPECT_EQ(f.model.carrier_of("Weapon &"), "Weapon");
    EXPECT_EQ(f.model.carrier_of("const Weapon&"), "Weapon");
    EXPECT_EQ(f.model.carrier_of("std::shared_ptr<Weapon>"), "Weapon");
    EXPECT_EQ(f.model.carrier_of("const std::shared_ptr<Weapon> &"), "Weapon");
    EXPECT_EQ(f.model.carrier_of("std::shared_ptr<Weapon>&&"), "Weapon");
}

TEST(CarrierOf, NotCarriers)
{
    CarrierFixture f;
    EXPECT_EQ(f.model.carrier_of("Weapon"), "");
    EXPECT_EQ(f.model.carrier_of("const Weapon"), "");
    EXPECT_EQ(f.model.carrier_of("std::shared_ptr<Weapon>&"), "");
    EXPECT_EQ(f.model.carrier_of("const std::shared_ptr<Weapon>"), "");
    EXPECT_EQ(f.model.carrier_of("Plain*"), "");
    EXPECT_EQ(f.model.carrier_of("int"), "");
    EXPECT_EQ(f.model.carrier_of("int*"), "");
    EXPECT_EQ(f.model.carrier_of("std::shared_ptr<Plain>"), "");
    EXPECT_EQ(f.model.carrier_of("Weapon**"), "");
    EXPECT_EQ(f.model.carrier_of(""), "");
}

TEST(CarrierOf, IntrusiveHandle)
{
    CarrierFixture f;
    EXPECT_EQ(f.model.carrier_of("Item*"), "Item");
    EXPECT_EQ(f.model.carrier_of("const Item*"), "Item");
    EXPECT_EQ(f.model.carrier_of("Item&"), "Item");
    EXPECT_EQ(f.model.carrier_of("Item"), "");
}

TEST(CarrierOf, ScriptClass)
{
    CarrierFixture f;
    std::vector<ast::ClassDef> classes(1);
    classes[0].name = "Bullet";
    f.model.add_script_classes(classes);
    EXPECT_EQ(f.model.carrier_of("Bullet*"), "Bullet");
    EXPECT_EQ(f.model.carrier_of("const std::shared_ptr<Bullet>&"), "Bullet");
}

TEST(MatchNativeVirtual, ExactWins)
{
    CarrierFixture f;
    // Both methods could match "Weapon": the exact spelling is the only exact hit.
    std::vector<VirtualSig> v = {sig("hit", {"Weapon*"}), sig("hit", {"Weapon"})};
    OverrideMatch m = match_native_virtual("hit", {"Weapon*"}, v, f.model, false, true);
    ASSERT_EQ(m.kind, OverrideMatch::Kind::Hit);
    EXPECT_FALSE(m.relaxed);
    EXPECT_EQ(m.hits, std::vector<size_t>({0}));
}

TEST(MatchNativeVirtual, ExactBuiltinCanonical)
{
    CarrierFixture f;
    std::vector<VirtualSig> v = {sig("f", {"unsigned int"})};
    OverrideMatch m = match_native_virtual("f", {"uint32"}, v, f.model, false, false);
    EXPECT_EQ(m.kind, OverrideMatch::Kind::Hit);
}

TEST(MatchNativeVirtual, RelaxedSingleHit)
{
    CarrierFixture f;
    std::vector<VirtualSig> v = {sig("hit", {"const Weapon&", "int"}), sig("other", {"Weapon*"})};
    OverrideMatch m = match_native_virtual("hit", {"Weapon", "int"}, v, f.model, false, true);
    ASSERT_EQ(m.kind, OverrideMatch::Kind::Hit);
    EXPECT_TRUE(m.relaxed);
    EXPECT_EQ(m.hits, std::vector<size_t>({0}));
}

TEST(MatchNativeVirtual, RelaxedOff)
{
    CarrierFixture f;
    std::vector<VirtualSig> v = {sig("hit", {"Weapon*"})};
    OverrideMatch m = match_native_virtual("hit", {"Weapon"}, v, f.model, false, false);
    EXPECT_EQ(m.kind, OverrideMatch::Kind::None);
    EXPECT_EQ(m.relaxed_candidates, std::vector<size_t>({0}));
}

TEST(MatchNativeVirtual, RelaxedAmbiguity)
{
    CarrierFixture f;
    std::vector<VirtualSig> v = {sig("hit", {"Weapon*"}), sig("hit", {"std::shared_ptr<Weapon>"})};
    OverrideMatch m = match_native_virtual("hit", {"Weapon"}, v, f.model, false, true);
    ASSERT_EQ(m.kind, OverrideMatch::Kind::Ambiguous);
    EXPECT_TRUE(m.relaxed);
    EXPECT_EQ(m.hits.size(), 2u);
}

TEST(MatchNativeVirtual, NoHit)
{
    CarrierFixture f;
    std::vector<VirtualSig> v = {sig("hit", {"Weapon*"}), sig("hit", {"int"})};
    OverrideMatch m = match_native_virtual("hit", {"float"}, v, f.model, false, true);
    EXPECT_EQ(m.kind, OverrideMatch::Kind::None);
    EXPECT_TRUE(m.hits.empty());
    EXPECT_TRUE(m.relaxed_candidates.empty());
    m = match_native_virtual("miss", {"Weapon"}, v, f.model, false, true);
    EXPECT_EQ(m.kind, OverrideMatch::Kind::None);
}

TEST(MatchNativeVirtual, ConstSelection)
{
    CarrierFixture f;
    std::vector<VirtualSig> v = {sig("get", {"int"}, true), sig("get", {"int"}, false)};
    OverrideMatch m = match_native_virtual("get", {"int"}, v, f.model, false, false);
    ASSERT_EQ(m.kind, OverrideMatch::Kind::Hit);
    EXPECT_EQ(m.hits, std::vector<size_t>({1}));
    m = match_native_virtual("get", {"int"}, v, f.model, true, false);
    ASSERT_EQ(m.kind, OverrideMatch::Kind::Hit);
    EXPECT_EQ(m.hits, std::vector<size_t>({0}));
    // A lone const method matches without the marker.
    std::vector<VirtualSig> c = {sig("get", {"int"}, true)};
    m = match_native_virtual("get", {"int"}, c, f.model, false, false);
    ASSERT_EQ(m.kind, OverrideMatch::Kind::Hit);
    // The marker with only a non-const method is an error.
    std::vector<VirtualSig> n = {sig("get", {"int"}, false)};
    m = match_native_virtual("get", {"int"}, n, f.model, true, false);
    EXPECT_EQ(m.kind, OverrideMatch::Kind::ConstMismatch);
}

TEST(MatchNativeVirtual, RelaxedConstSelection)
{
    CarrierFixture f;
    std::vector<VirtualSig> v = {sig("use", {"Weapon*"}, true), sig("use", {"Weapon*"}, false)};
    OverrideMatch m = match_native_virtual("use", {"Weapon"}, v, f.model, true, true);
    ASSERT_EQ(m.kind, OverrideMatch::Kind::Hit);
    EXPECT_TRUE(m.relaxed);
    EXPECT_EQ(m.hits, std::vector<size_t>({0}));
    m = match_native_virtual("use", {"Weapon"}, v, f.model, false, true);
    ASSERT_EQ(m.kind, OverrideMatch::Kind::Hit);
    EXPECT_EQ(m.hits, std::vector<size_t>({1}));
}

TEST(MatchNativeVirtual, RelaxedIsOn)
{
    EXPECT_TRUE(relaxed_override_enabled());
}

TEST(SharedFromThisFact, BaseWalk)
{
    CarrierFixture f;
    std::vector<ast::ClassDef> classes(1);
    classes[0].name = "Bullet";
    classes[0].base = "Derived";
    f.model.add_script_classes(classes);
    f.model.set_bases(bases_of({{"Derived", {"Mid"}}, {"Mid", {"Root"}}}));
    f.model.set_classes(
        [](const std::string& name)
        {
            TypeModel::ClassFacts c;
            c.known = true;
            c.shared_from_this = name == "Root";
            return c;
        });
    EXPECT_TRUE(f.model.has_shared_from_this("Root"));
    EXPECT_TRUE(f.model.has_shared_from_this("Derived"));
    EXPECT_TRUE(f.model.has_shared_from_this("Bullet"));
    EXPECT_FALSE(f.model.has_shared_from_this("Other"));
}

TEST(OverrideReturn, ByValueHandleIsRelaxed)
{
    CarrierFixture f;
    EXPECT_TRUE(override_return_ok("Weapon", "std::shared_ptr<Weapon>", f.model, true));
    EXPECT_FALSE(override_return_ok("Weapon", "std::shared_ptr<Weapon>", f.model, false));
    EXPECT_TRUE(override_return_ok("int", "int", f.model, false));
    EXPECT_TRUE(override_return_ok("uint32", "unsigned int", f.model, true));
}

TEST(OverrideReturn, PointerAndReferenceAreNotRelaxed)
{
    CarrierFixture f;
    EXPECT_FALSE(override_return_ok("Weapon", "Weapon*", f.model, true));
    EXPECT_FALSE(override_return_ok("Weapon", "Weapon&", f.model, true));
    EXPECT_FALSE(override_return_ok("Weapon", "const Weapon&", f.model, true));
    EXPECT_FALSE(override_return_ok("Weapon", "const std::shared_ptr<Weapon>&", f.model, true));
    // The intrusive "T*" is a handle for a parameter, not for a return.
    EXPECT_FALSE(override_return_ok("Item", "Item*", f.model, true));
    EXPECT_EQ(f.model.return_carrier_of("std::shared_ptr<Weapon>"), "Weapon");
    EXPECT_EQ(f.model.return_carrier_of("Item*"), "");
}

TEST(OverrideReturn, OtherHandleTypeDiffers)
{
    CarrierFixture f;
    EXPECT_FALSE(override_return_ok("Item", "std::shared_ptr<Weapon>", f.model, true));
}
