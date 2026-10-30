// Unit tests of Loader::reset (full reload) with hand-written records and no backend.
#include "spp/runtime.h"
#include "support/reload_assert.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>

namespace
{

struct Fake : spp::ShellBase
{
    virtual ~Fake() { spp::destroy_state(this); }
};
struct Other : spp::ShellBase
{
    virtual ~Other() { spp::destroy_state(this); }
};

struct State1
{
    int value;
};
spp::FieldInfo fields1[] = {
    {"value", nullptr, spp::type_key<int>(), offsetof(State1, value), sizeof(int), alignof(int),
        spp::field_flags<int>(), [](void*, void* f) { new (f) int(1); }, spp::move_field<int>,
        spp::field_destroyer<int>()},
};
spp::StateLayout layout1{"State1", sizeof(State1), alignof(State1), 1, fields1};

spp::SlotFn slots[1] = {nullptr};
constexpr uint64_t kHash = spp::shell_layout_hash("Base", sizeof(Fake), alignof(Fake), 0);

spp::ClassRecord fake1{"Fake", 1, slots, 1, &layout1, nullptr, "Base", kHash};
spp::ClassRecord fake2{"Fake", 2, slots, 1, &layout1, nullptr, "Base", kHash};
spp::ClassRecord other1{"Other", 1, slots, 1, &layout1, nullptr, "Base", kHash};
spp::ClassRecord other2{"Other", 2, slots, 1, &layout1, nullptr, "Base", kHash};

template <class T> spp::Instance make(spp::ClassRecord* rec)
{
    auto* s = new T;
    spp::init_state(s, rec);
    return {s, s};
}
spp::Instance create_fake1()
{
    return make<Fake>(&fake1);
}
spp::Instance create_fake2()
{
    return make<Fake>(&fake2);
}
spp::Instance create_other1()
{
    return make<Other>(&other1);
}
spp::Instance create_other2()
{
    return make<Other>(&other2);
}

void register1(spp::Registry& reg)
{
    reg.add(&fake1, &create_fake1);
    reg.add(&other1, &create_other1);
}
void register2(spp::Registry& reg)
{
    reg.add(&fake2, &create_fake2);
    reg.add(&other2, &create_other2);
}

spp::Module module_of(spp::Module::RegisterFn fn, const char* name)
{
    spp::Module m;
    m.register_fn = fn;
    m.origin = name;
    return m;
}

struct Reset : ::testing::Test
{
    // The records change in a commit. Each test starts from new ones.
    Reset()
    {
        fake1 = spp::ClassRecord{"Fake", 1, slots, 1, &layout1, nullptr, "Base", kHash};
        fake2 = spp::ClassRecord{"Fake", 2, slots, 1, &layout1, nullptr, "Base", kHash};
        other1 = spp::ClassRecord{"Other", 1, slots, 1, &layout1, nullptr, "Base", kHash};
        other2 = spp::ClassRecord{"Other", 2, slots, 1, &layout1, nullptr, "Base", kHash};
    }
    spp::Loader loader;
};

TEST_F(Reset, LiveObjectBlocksResetAndTheErrorNamesTheClassAndTheCount)
{
    ASSERT_RELOAD_OK(loader.load(module_of(register1, "gen1")));
    spp::Instance a = loader.find("Fake")->create();
    spp::Instance b = loader.find("Fake")->create();
    spp::Instance c = loader.find("Fake")->create();
    spp::Instance d = loader.find("Other")->create();
    std::string error;
    EXPECT_FALSE(loader.reset(error));
    EXPECT_EQ(error, "4 live objects remain: Fake x3, Other x1");
    EXPECT_EQ(loader.stats().live_objects, 4u); // nothing changed
    EXPECT_EQ(loader.stats().active_generation, 1u);
    delete static_cast<Fake*>(a.shell);
    delete static_cast<Fake*>(b.shell);
    delete static_cast<Fake*>(c.shell);
    delete static_cast<Other*>(d.shell);
    EXPECT_TRUE(loader.reset(error)) << error;
}

TEST_F(Reset, ResetAfterTheReleaseClearsTheStats)
{
    ASSERT_RELOAD_OK(loader.load(module_of(register1, "gen1")));
    spp::Instance a = loader.find("Fake")->create();
    delete static_cast<Fake*>(a.shell);
    std::string error;
    ASSERT_TRUE(loader.reset(error)) << error;
    const spp::LoaderStats st = loader.stats();
    EXPECT_EQ(st.active_generation, 0u);
    EXPECT_TRUE(st.generations.empty());
    EXPECT_TRUE(st.classes.empty());
    EXPECT_EQ(st.slots, 0u);
    EXPECT_EQ(st.core_modules, 0u);
    EXPECT_EQ(loader.find("Fake"), nullptr);
}

TEST_F(Reset, LoadAgainAtGenerationOneWorks)
{
    ASSERT_RELOAD_OK(loader.load(module_of(register1, "gen1")));
    std::string error;
    ASSERT_TRUE(loader.reset(error)) << error;
    ASSERT_RELOAD_OK(loader.load(module_of(register1, "gen1 again")));
    const spp::LoaderStats st = loader.stats();
    EXPECT_EQ(st.active_generation, 1u);
    ASSERT_EQ(st.generations.size(), 1u);
    EXPECT_EQ(st.generations[0].generation, 1u);
    EXPECT_EQ(st.generations[0].origin, "gen1 again");
    ASSERT_NE(loader.find("Fake"), nullptr);
    spp::Instance a = loader.find("Fake")->create();
    delete static_cast<Fake*>(a.shell);
}

TEST_F(Reset, PendingReloadBlocksReset)
{
    ASSERT_RELOAD_OK(loader.load(module_of(register1, "gen1")));
    ASSERT_RELOAD_OK(loader.request_reload(module_of(register2, "gen2")));
    ASSERT_TRUE(loader.reload_pending());
    std::string error;
    EXPECT_FALSE(loader.reset(error));
    EXPECT_EQ(error, "a reload is pending");
    EXPECT_TRUE(loader.reload_pending()); // the reload stays
    ASSERT_RELOAD_OK(loader.commit_pending());
    EXPECT_TRUE(loader.reset(error)) << error;
    EXPECT_EQ(loader.stats().active_generation, 0u);
}

TEST_F(Reset, PinnedGenerationBlocksReset)
{
    ASSERT_RELOAD_OK(loader.load(module_of(register1, "gen1")));
    spp::pin(1);
    std::string error;
    EXPECT_FALSE(loader.reset(error));
    EXPECT_EQ(error, "generation 1 is pinned");
    EXPECT_EQ(loader.stats().active_generation, 1u);
    spp::unpin(1);
    EXPECT_TRUE(loader.reset(error)) << error;
}

TEST_F(Reset, ResetOfAnEmptyLoaderWorks)
{
    std::string error;
    EXPECT_TRUE(loader.reset(error)) << error;
    ASSERT_RELOAD_OK(loader.load(module_of(register1, "gen1")));
}

} // namespace
