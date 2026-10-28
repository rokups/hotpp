// Unit tests of the module state block (design 5.3, 4.7) with hand-written records and no backend.
// The block migrates like the fields of a class: by name, by alias, with a convert hook.
#include <gtest/gtest.h>

#include <cstddef>
#include <string>

#include "spp/runtime.h"

namespace {

struct Mod1 {
    int count;
    float speed;
    int tag;
    int gone;
};
struct Mod2 {
    int total;  // the alias of "count"
    float speed;
    float tag;  // the type changes: the convert hook runs
    int extra;  // new
};

template <class T>
spp::FieldInfo mk(const char* name, const char* alias, uint32_t off, void (*init)(void*, void*))
{
    return {name, alias, spp::type_key<T>(), off, sizeof(T), alignof(T), spp::field_flags<T>(),
            init, spp::move_field<T>, spp::field_destroyer<T>()};
}

spp::FieldInfo mfields1[] = {
    mk<int>("count", nullptr, offsetof(Mod1, count), [](void*, void* f) { new (f) int(1); }),
    mk<float>("speed", nullptr, offsetof(Mod1, speed), [](void*, void* f) { new (f) float(2.0f); }),
    mk<int>("tag", nullptr, offsetof(Mod1, tag), [](void*, void* f) { new (f) int(3); }),
    mk<int>("gone", nullptr, offsetof(Mod1, gone), [](void*, void* f) { new (f) int(4); }),
};
spp::StateLayout mlayout1{"Mod1", sizeof(Mod1), alignof(Mod1), 4, mfields1};

spp::FieldInfo mfields2[] = {
    mk<int>("total", "count", offsetof(Mod2, total), [](void*, void* f) { new (f) int(-1); }),
    mk<float>("speed", nullptr, offsetof(Mod2, speed), [](void*, void* f) { new (f) float(-2.0f); }),
    mk<float>("tag", nullptr, offsetof(Mod2, tag), [](void*, void* f) { new (f) float(-3.0f); }),
    mk<int>("extra", nullptr, offsetof(Mod2, extra), [](void*, void* f) { new (f) int(9); }),
};
spp::StateLayout mlayout2{"Mod2", sizeof(Mod2), alignof(Mod2), 4, mfields2};

// Each generation has its own slot variable, as the generated code has.
void* slot1 = nullptr;
void* slot2 = nullptr;

// A class record gives the generation number. It has no fields.
spp::StateLayout empty_layout{"Empty", 1, 1, 0, nullptr};
void* cslots[1] = {nullptr};
constexpr uint64_t kHash = spp::shell_layout_hash("Base", 1, 1, 0);
spp::ClassRecord crec1{"Empty", 1, cslots, 1, &empty_layout, nullptr, "Base", kHash};
spp::ClassRecord crec2{"Empty", 2, cslots, 1, &empty_layout, nullptr, "Base", kHash};
spp::Instance make_none() { return {nullptr, nullptr}; }

void register1(spp::Registry& reg)
{
    reg.add(&crec1, &make_none);
    reg.set_module_state(&mlayout1, &slot1);
}
void register2(spp::Registry& reg)
{
    reg.add(&crec2, &make_none);
    reg.set_module_state(&mlayout2, &slot2);
}
void register_without(spp::Registry& reg) { reg.add(&crec2, &make_none); }
void register_without1(spp::Registry& reg) { reg.add(&crec1, &make_none); }

spp::Module module_of(spp::Module::RegisterFn fn, const char* name)
{
    spp::Module m;
    m.register_fn = fn;
    m.origin = name;
    return m;
}

struct Scenario {
    spp::Loader loader;
    Scenario()
    {
        crec1 = spp::ClassRecord{"Empty", 1, cslots, 1, &empty_layout, nullptr, "Base", kHash};
        mfields2[2].convert = nullptr;
        mfields2[2].convert_from = nullptr;
        slot1 = slot2 = nullptr;
    }
    void start()
    {
        std::string error;
        ASSERT_TRUE(loader.load(module_of(register1, "gen1"), error)) << error;
    }
};

}  // namespace

TEST(ModuleState, LoadMakesTheBlockWithTheInitializers)
{
    Scenario s;
    ASSERT_NO_FATAL_FAILURE(s.start());
    ASSERT_NE(slot1, nullptr);
    const auto* m = static_cast<Mod1*>(slot1);
    EXPECT_EQ(m->count, 1);
    EXPECT_EQ(m->speed, 2.0f);
    EXPECT_EQ(m->tag, 3);
    EXPECT_EQ(s.loader.module_state().layout, &mlayout1);
}

TEST(ModuleState, ReloadMigratesByNameAliasAndConvertHook)
{
    Scenario s;
    ASSERT_NO_FATAL_FAILURE(s.start());
    auto* m1 = static_cast<Mod1*>(slot1);
    m1->count = 40;
    m1->speed = 7.5f;
    m1->tag = 6;
    mfields2[2].convert = [](void* dst, const void* src) {
        new (dst) float(static_cast<float>(*static_cast<const int*>(src)) * 10.0f);
    };
    mfields2[2].convert_from = spp::type_key<int>();

    std::string error;
    ASSERT_TRUE(s.loader.request_reload(module_of(register2, "gen2"), error)) << error;
    testing::internal::CaptureStderr();
    ASSERT_TRUE(s.loader.commit_pending(error)) << error;
    testing::internal::GetCapturedStderr();

    EXPECT_EQ(slot1, nullptr);  // the old generation has no block now
    ASSERT_NE(slot2, nullptr);
    const auto* m2 = static_cast<Mod2*>(slot2);
    EXPECT_EQ(m2->total, 40);       // alias
    EXPECT_EQ(m2->speed, 7.5f);     // name
    EXPECT_EQ(m2->tag, 60.0f);      // convert hook
    EXPECT_EQ(m2->extra, 9);        // new variable: the initializer
    EXPECT_EQ(s.loader.module_state().layout, &mlayout2);
}

TEST(ModuleState, ATypeChangeWithoutAHookWarnsAndTakesTheDefault)
{
    Scenario s;
    ASSERT_NO_FATAL_FAILURE(s.start());
    std::string error;
    ASSERT_TRUE(s.loader.request_reload(module_of(register2, "gen2"), error)) << error;
    testing::internal::CaptureStderr();
    ASSERT_TRUE(s.loader.commit_pending(error)) << error;
    const std::string log = testing::internal::GetCapturedStderr();
    EXPECT_NE(log.find("field tag changed its type"), std::string::npos) << log;
    EXPECT_EQ(static_cast<Mod2*>(slot2)->tag, -3.0f);
    EXPECT_EQ(spp::active_generation, 2u);
}

TEST(ModuleState, AReloadWithoutModuleVariablesDropsTheBlock)
{
    Scenario s;
    ASSERT_NO_FATAL_FAILURE(s.start());
    std::string error;
    ASSERT_TRUE(s.loader.request_reload(module_of(register_without, "gen2"), error)) << error;
    testing::internal::CaptureStderr();
    ASSERT_TRUE(s.loader.commit_pending(error)) << error;
    testing::internal::GetCapturedStderr();
    EXPECT_EQ(slot1, nullptr);
    EXPECT_EQ(s.loader.module_state().layout, nullptr);
}

TEST(ModuleState, AReloadFromNoBlockInitializesAllVariables)
{
    Scenario s;
    std::string error;
    ASSERT_TRUE(s.loader.load(module_of(register_without1, "gen1"), error)) << error;
    EXPECT_EQ(s.loader.module_state().layout, nullptr);
    crec2 = spp::ClassRecord{"Empty", 2, cslots, 1, &empty_layout, nullptr, "Base", kHash};
    ASSERT_TRUE(s.loader.request_reload(module_of(register2, "gen2"), error)) << error;
    testing::internal::CaptureStderr();
    ASSERT_TRUE(s.loader.commit_pending(error)) << error;
    testing::internal::GetCapturedStderr();
    ASSERT_NE(slot2, nullptr);
    EXPECT_EQ(static_cast<Mod2*>(slot2)->total, -1);
    EXPECT_EQ(static_cast<Mod2*>(slot2)->extra, 9);
}
