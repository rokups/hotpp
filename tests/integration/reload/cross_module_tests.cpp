// Reload tests of a script family over two modules (S20a, design 4.5 and 4.10). The child module
// imports the parent module with "using "xmod_parent.spp";". One generation holds both modules.
// A reload of either module builds a new generation from the current text of both modules.
#include <gtest/gtest.h>

#include <string>

#include "spp/backend.h"
#include "support/backend_fixture.h"
#include "support/script_files.h"
#include "support/test_support.h"

namespace {

using test_support::kDt;
using test_support::Session;

template <class T>
T* fld(Session& s, const char* name)
{
    return spp::field_ptr<T>(s.inst.shell, name);
}

// The child module with the text of a parent module under the name of the "using" line.
spp::ScriptSource family(const char* child, const char* parent)
{
    spp::ScriptSource src = test_support::source(child);
    src.modules.emplace_back("xmod_parent.spp", test_support::source(parent).text);
    return src;
}

void reload(Session& s, spp::LoadResult& r, uint32_t gen)
{
    ASSERT_TRUE(r) << r.error;
    std::string error;
    ASSERT_TRUE(s.loader.request_reload(std::move(*r.module), error)) << error;
    s.world.Tick(kDt);  // the commit happens at the end of this frame
    ASSERT_FALSE(s.commit_failed);
    EXPECT_EQ(spp::active_generation, gen);
}

// A reload of the parent module, then a reload of the child module. The live child object keeps
// its address and its values. The child override stays in its slot after the parent adds a slot.
TEST_P(BackendTests, FamilyOverTwoModulesReloadsEachModule)
{
    spp::LoadResult g1 = backend->load(family("xmod_child_v1.spp", "xmod_parent_v1.spp"), 1);
    ASSERT_TRUE(g1) << g1.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*g1.module), "Grunt"));
    s.commit_at_frame_boundary();
    ASSERT_NE(s.inst.shell->cls->super, nullptr);
    EXPECT_STREQ(s.inst.shell->cls->super->name, "Unit");
    EXPECT_EQ(*fld<int>(s, "ctor_log"), 12);  // Unit(int) of the parent module first, then Grunt(int)
    EXPECT_EQ(*fld<int>(s, "hp"), 1);
    *fld<int>(s, "rage") = 7;
    s.world.Tick(kDt);
    EXPECT_EQ(*fld<int>(s, "ticks"), 1);
    EXPECT_EQ(*fld<int>(s, "power"), 7);  // the parent Tick calls the child override
    void* const shell = s.inst.shell;

    // Reload the parent module: a new field and a new virtual function before "Power".
    spp::LoadResult g2 = backend->load(family("xmod_child_v1.spp", "xmod_parent_v2.spp"), 2);
    ASSERT_NO_FATAL_FAILURE(reload(s, g2, 2));
    s.world.Tick(kDt);
    EXPECT_EQ(s.inst.shell, shell);
    EXPECT_EQ(*fld<int>(s, "power"), 107);  // the Tick of parent v2 and the Power of child v1
    EXPECT_EQ(*fld<int>(s, "bonus"), 5);    // the new parent virtual
    EXPECT_EQ(*fld<int>(s, "armor"), 9);    // the new parent field of the old child object
    EXPECT_EQ(*fld<int>(s, "hp"), 1);
    EXPECT_EQ(*fld<int>(s, "rage"), 7);
    EXPECT_EQ(s.inst.shell->cls->super->generation, 2u);

    // Reload the child module: a changed override, a new override and a new field.
    spp::LoadResult g3 = backend->load(family("xmod_child_v2.spp", "xmod_parent_v2.spp"), 3);
    ASSERT_NO_FATAL_FAILURE(reload(s, g3, 3));
    s.world.Tick(kDt);
    EXPECT_EQ(s.inst.shell, shell);
    EXPECT_EQ(*fld<int>(s, "power"), 114);  // rage * 2 + 100
    EXPECT_EQ(s.inst.shell->cls->slot_count, 5u);
    EXPECT_EQ(*fld<int>(s, "bonus"), 4);    // the child override of Bonus reads the new field
    EXPECT_EQ(*fld<int>(s, "armor"), 9);
    EXPECT_EQ(*fld<int>(s, "ticks"), 5);  // one tick in each frame, also in the two commit frames
}

// The child module builds a child object with "new" and uses it through a parent handle.
TEST_P(BackendTests, FamilyOverTwoModulesNewAndIs)
{
    spp::LoadResult g1 = backend->load(family("xmod_child_v1.spp", "xmod_parent_v1.spp"), 1);
    ASSERT_TRUE(g1) << g1.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*g1.module), "Spawner"));
    s.commit_at_frame_boundary();
    s.world.Tick(kDt);
    EXPECT_EQ(*fld<int>(s, "made_power"), 6);
    EXPECT_TRUE(*fld<bool>(s, "made_unit"));
    spp::LoadResult g2 = backend->load(family("xmod_child_v2.spp", "xmod_parent_v2.spp"), 2);
    ASSERT_NO_FATAL_FAILURE(reload(s, g2, 2));
    s.world.Tick(kDt);
    EXPECT_EQ(*fld<int>(s, "made_power"), 12);
}

// A missing parent module is a translation error at the "using" line.
TEST_P(BackendTests, FamilyOverTwoModulesMissingParent)
{
    spp::LoadResult r = backend->load(test_support::source("xmod_child_v1.spp"), 1);
    ASSERT_FALSE(r);
    EXPECT_NE(r.error.find("the script 'xmod_parent.spp' is not found"), std::string::npos) << r.error;
}

// Constructor overloads, a "ref" constructor parameter and parent overloads from "super(...)".
// Generation 2 uses "new" for a class without a constructor and without "new" in generation 1.
TEST_P(BackendTests, ConstructorOverloadsAndALaterNew)
{
    spp::LoadResult g1 = backend->load(test_support::source("ctor_overload_v1.spp"), 1);
    ASSERT_TRUE(g1) << g1.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*g1.module), "Spawner"));
    s.commit_at_frame_boundary();
    s.world.Tick(kDt);
    EXPECT_EQ(*fld<int>(s, "k0"), 1);
    EXPECT_EQ(*fld<int>(s, "k1"), 7);
    EXPECT_EQ(*fld<int>(s, "k2"), 3);
    EXPECT_EQ(*fld<int>(s, "k3"), 4);
    EXPECT_EQ(*fld<int>(s, "seen"), 8);
    EXPECT_EQ(*fld<int>(s, "elite"), 309);  // Elite() selects Unit(float), Elite(8) selects Unit(int)
    spp::LoadResult g2 = backend->load(test_support::source("ctor_overload_v2.spp"), 2);
    ASSERT_NO_FATAL_FAILURE(reload(s, g2, 2));
    s.world.Tick(kDt);
    EXPECT_EQ(*fld<int>(s, "plain"), 3);
}

// A "super(...)" argument binds to a "ref" or "out" parameter of a script parent (S23). The
// "ref" reaches the parent through two levels. A "&&" constructor parameter takes a temporary.
TEST_P(BackendTests, SuperArgumentBindsToARefParameter)
{
    spp::LoadResult g1 = backend->load(test_support::source("ctor_ref_v1.spp"), 1);
    ASSERT_TRUE(g1) << g1.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*g1.module), "Spawner"));
    s.commit_at_frame_boundary();
    s.world.Tick(kDt);
    EXPECT_EQ(*fld<int>(s, "mid_hp"), 5);
    EXPECT_EQ(*fld<int>(s, "seen"), 10);  // Base wrote 5 * 2 into the "ref" of Mid
    EXPECT_EQ(*fld<int>(s, "leaf_hp"), 12);
    EXPECT_EQ(*fld<int>(s, "leaf_seen"), 24);
    EXPECT_EQ(*fld<int>(s, "made"), 100);  // "out" through Wrap
    EXPECT_EQ(*fld<std::string>(s, "named"), "abc");
}

}  // namespace
