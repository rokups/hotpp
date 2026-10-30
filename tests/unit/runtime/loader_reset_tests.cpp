// Unit tests of Loader::reset (full reload) with hand-written records and no backend.
#include "hot/runtime.h"
#include "support/reload_assert.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>

namespace
{

struct Fake : hot::ShellBase
{
    virtual ~Fake() { hot::destroy_state(this); }
};
struct Other : hot::ShellBase
{
    virtual ~Other() { hot::destroy_state(this); }
};

struct State1
{
    int value;
};
hot::FieldInfo fields1[] = {
    {"value", nullptr, hot::type_key<int>(), offsetof(State1, value), sizeof(int), alignof(int),
        hot::field_flags<int>(), [](void*, void* f) { new (f) int(1); }, hot::move_field<int>,
        hot::field_destroyer<int>()},
};
hot::StateLayout layout1{"State1", sizeof(State1), alignof(State1), 1, fields1};

hot::SlotFn slots[1] = {nullptr};
constexpr uint64_t kHash = hot::shell_layout_hash("Base", sizeof(Fake), alignof(Fake), 0);

hot::ClassRecord fake1{"Fake", 1, slots, 1, &layout1, nullptr, "Base", kHash};
hot::ClassRecord fake2{"Fake", 2, slots, 1, &layout1, nullptr, "Base", kHash};
hot::ClassRecord other1{"Other", 1, slots, 1, &layout1, nullptr, "Base", kHash};
hot::ClassRecord other2{"Other", 2, slots, 1, &layout1, nullptr, "Base", kHash};

template <class T> hot::Instance make(hot::ClassRecord* rec)
{
    auto* s = new T;
    hot::init_state(s, rec);
    return {s, s};
}
hot::Instance create_fake1()
{
    return make<Fake>(&fake1);
}
hot::Instance create_fake2()
{
    return make<Fake>(&fake2);
}
hot::Instance create_other1()
{
    return make<Other>(&other1);
}
hot::Instance create_other2()
{
    return make<Other>(&other2);
}

void register1(hot::Registry& reg)
{
    reg.add(&fake1, &create_fake1);
    reg.add(&other1, &create_other1);
}
void register2(hot::Registry& reg)
{
    reg.add(&fake2, &create_fake2);
    reg.add(&other2, &create_other2);
}

hot::Module module_of(hot::Module::RegisterFn fn, const char* name)
{
    hot::Module m;
    m.register_fn = fn;
    m.origin = name;
    return m;
}

struct Reset : ::testing::Test
{
    // The records change in a commit. Each test starts from new ones.
    Reset()
    {
        fake1 = hot::ClassRecord{"Fake", 1, slots, 1, &layout1, nullptr, "Base", kHash};
        fake2 = hot::ClassRecord{"Fake", 2, slots, 1, &layout1, nullptr, "Base", kHash};
        other1 = hot::ClassRecord{"Other", 1, slots, 1, &layout1, nullptr, "Base", kHash};
        other2 = hot::ClassRecord{"Other", 2, slots, 1, &layout1, nullptr, "Base", kHash};
    }
    hot::Loader loader;
};

TEST_F(Reset, LiveObjectBlocksResetAndTheErrorNamesTheClassAndTheCount)
{
    ASSERT_RELOAD_OK(loader.load(module_of(register1, "gen1")));
    hot::Instance a = loader.find("Fake")->create();
    hot::Instance b = loader.find("Fake")->create();
    hot::Instance c = loader.find("Fake")->create();
    hot::Instance d = loader.find("Other")->create();
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
    hot::Instance a = loader.find("Fake")->create();
    delete static_cast<Fake*>(a.shell);
    std::string error;
    ASSERT_TRUE(loader.reset(error)) << error;
    const hot::LoaderStats st = loader.stats();
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
    const hot::LoaderStats st = loader.stats();
    EXPECT_EQ(st.active_generation, 1u);
    ASSERT_EQ(st.generations.size(), 1u);
    EXPECT_EQ(st.generations[0].generation, 1u);
    EXPECT_EQ(st.generations[0].origin, "gen1 again");
    ASSERT_NE(loader.find("Fake"), nullptr);
    hot::Instance a = loader.find("Fake")->create();
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
    hot::pin(1);
    std::string error;
    EXPECT_FALSE(loader.reset(error));
    EXPECT_EQ(error, "generation 1 is pinned");
    EXPECT_EQ(loader.stats().active_generation, 1u);
    hot::unpin(1);
    EXPECT_TRUE(loader.reset(error)) << error;
}

TEST_F(Reset, ResetOfAnEmptyLoaderWorks)
{
    std::string error;
    EXPECT_TRUE(loader.reset(error)) << error;
    ASSERT_RELOAD_OK(loader.load(module_of(register1, "gen1")));
}

} // namespace
