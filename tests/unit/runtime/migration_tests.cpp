// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Unit tests of the migration (design 4.7 and 6.6) with hand-written records and no backend.
// The tests cover the plan, the convert hook, the onReload hook with OldState,
// the warnings, the rejections that stay and the restart request.
#include "hot/runtime.h"
#include "support/reload_assert.h"
#include "../../../src/runtime/runtime_internal.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace
{

// ---- the fake script class "Fake": one generation 1 and one generation 2 in the same test file
struct Fake : hot::ShellBase
{
    virtual ~Fake() { hot::destroy_state(this); }
};

struct State1
{
    float hp;
    int level;
    float mana;
    int tag;
};
struct State2
{
    float health;
    float level;
    float energy;
    float tag;
    int extra;
};

template <class T> hot::FieldInfo mk(const char* name, const char* alias, uint32_t off, void (*init)(void*, void*))
{
    return {name, alias, hot::type_key<T>(), off, sizeof(T), alignof(T), hot::field_flags<T>(), init,
        hot::move_field<T>, hot::field_destroyer<T>()};
}

hot::FieldInfo fields1[] = {
    mk<float>("hp", nullptr, offsetof(State1, hp), [](void*, void* f) { new (f) float(100.0f); }),
    mk<int>("level", nullptr, offsetof(State1, level), [](void*, void* f) { new (f) int(1); }),
    mk<float>("mana", nullptr, offsetof(State1, mana), [](void*, void* f) { new (f) float(10.0f); }),
    mk<int>("tag", nullptr, offsetof(State1, tag), [](void*, void* f) { new (f) int(5); }),
};
hot::StateLayout layout1{"State1", sizeof(State1), alignof(State1), 4, fields1};

float convert_level(int old)
{
    return static_cast<float>(old) * 10.0f;
}

hot::FieldInfo fields2[] = {
    mk<float>("health", "hp", offsetof(State2, health), [](void*, void* f) { new (f) float(50.0f); }),
    mk<float>("level", nullptr, offsetof(State2, level), [](void*, void* f) { new (f) float(1.5f); }),
    mk<float>("energy", nullptr, offsetof(State2, energy), [](void*, void* f) { new (f) float(5.0f); }),
    mk<float>("tag", nullptr, offsetof(State2, tag), [](void*, void* f) { new (f) float(2.5f); }),
    mk<int>("extra", nullptr, offsetof(State2, extra), [](void*, void* f) { new (f) int(7); }),
};
hot::StateLayout layout2{"State2", sizeof(State2), alignof(State2), 5, fields2};

hot::SlotFn slots[1] = {nullptr};
constexpr uint64_t kHash = hot::shell_layout_hash("Base", sizeof(Fake), alignof(Fake), 0);

hot::ClassRecord record1{"Fake", 1, slots, 1, &layout1, nullptr, "Base", kHash};
hot::ClassRecord record2{"Fake", 2, slots, 1, &layout2, nullptr, "Base", kHash};

hot::Instance create1()
{
    auto* s = new Fake;
    hot::init_state(s, &record1);
    return {s, s};
}
hot::Instance create2()
{
    auto* s = new Fake;
    hot::init_state(s, &record2);
    return {s, s};
}
void register1(hot::Registry& reg)
{
    reg.add(&record1, &create1);
}
void register2(hot::Registry& reg)
{
    reg.add(&record2, &create2);
}

// What the onReload hook saw.
struct HookLog
{
    int calls = 0;
    float old_hp = 0;
    bool has_hp = false, has_mana = false, has_nope = false, hp_is_float = false, hp_is_int = true;
    bool get_wrong_type_threw = false, get_missing_threw = false;
    int old_level = 0;
};
HookLog g_hook;
bool g_hook_throws = false;

void on_reload(hot::ShellBase* self, const hot::OldState& old)
{
    ++g_hook.calls;
    g_hook.has_hp = old.has("hp");
    g_hook.has_mana = old.has("mana");
    g_hook.has_nope = old.has("nope");
    g_hook.hp_is_float = old.has<float>("hp");
    g_hook.hp_is_int = old.has<int>("hp");
    g_hook.old_hp = old.get<float>("hp");
    g_hook.old_level = old.get<int>("level");
    try
    {
        old.get<int>("hp");
    }
    catch (const std::runtime_error&)
    {
        g_hook.get_wrong_type_threw = true;
    }
    try
    {
        old.get<int>("nope");
    }
    catch (const std::runtime_error&)
    {
        g_hook.get_missing_threw = true;
    }
    // The structural migration is done: the new fields are valid.
    *hot::field_ptr<float>(self, "energy") += 1.0f;
    if (g_hook_throws)
        throw std::runtime_error("boom");
}

hot::Module module_of(hot::Module::RegisterFn fn, const char* name)
{
    hot::Module m;
    m.register_fn = fn;
    m.origin = name;
    return m;
}

// The loader is the first member, so it is the last to go.
struct Scenario
{
    hot::Loader loader;
    Fake* obj = nullptr;
    Scenario()
    {
        // A commit changes the first record of a class. Each test starts from a new one.
        record1 = hot::ClassRecord{"Fake", 1, slots, 1, &layout1, nullptr, "Base", kHash};
        g_hook = HookLog{};
        g_hook_throws = false;
        record2.on_reload = nullptr;
        fields2[1].convert = nullptr;
        fields2[1].convert_from = nullptr;
    }
    void start()
    {
        std::string error;
        ASSERT_RELOAD_OK(loader.load(module_of(register1, "gen1")));
        hot::Instance inst = loader.find("Fake")->create();
        obj = static_cast<Fake*>(inst.shell);
        *hot::field_ptr<float>(obj, "hp") = 120.0f;
        *hot::field_ptr<int>(obj, "level") = 3;
        *hot::field_ptr<float>(obj, "mana") = 33.0f;
        *hot::field_ptr<int>(obj, "tag") = 9;
    }
    ~Scenario()
    {
        delete obj;
        record2.on_reload = nullptr;
    }
};

TEST(MigrationPlan, ConvertHookIsChosenOnlyForTheMatchingOldType)
{
    hot::FieldInfo of[] = {mk<int>("level", nullptr, 0, nullptr)};
    hot::FieldInfo nf[] = {mk<float>("level", nullptr, 0, nullptr)};
    hot::StateLayout o{"S", 4, 4, 1, of};
    hot::StateLayout n{"S", 4, 4, 1, nf};
    nf[0].convert = [](void* dst, const void* src)
    { new (dst) float(static_cast<float>(*static_cast<const int*>(src))); };
    nf[0].convert_from = hot::type_key<int>();
    hot::MigrationPlan p = hot::build_plan(o, n);
    EXPECT_EQ(p.steps[0].action, hot::FieldAction::Convert);
    EXPECT_TRUE(p.warnings.empty());
    EXPECT_EQ(p.changed, std::vector<uint32_t>{0});

    nf[0].convert_from = hot::type_key<double>(); // the hook takes another old type
    p = hot::build_plan(o, n);
    EXPECT_EQ(p.steps[0].action, hot::FieldAction::Init);
    EXPECT_EQ(p.warnings.size(), 1u);
}

TEST(MigrationPlan, RenameWithoutAliasWarnsAndAliasDoesNot)
{
    hot::FieldInfo of[] = {mk<float>("hp", nullptr, 0, nullptr)};
    hot::FieldInfo nf[] = {mk<float>("health", nullptr, 0, nullptr)};
    hot::StateLayout o{"Player", 4, 4, 1, of};
    hot::StateLayout n{"Player", 4, 4, 1, nf};
    hot::MigrationPlan p = hot::build_plan(o, n);
    EXPECT_EQ(p.steps[0].action, hot::FieldAction::Init);
    EXPECT_EQ(p.removed, std::vector<uint32_t>{0});
    ASSERT_EQ(p.warnings.size(), 1u);
    EXPECT_NE(p.warnings[0].find("hp is removed and field health is added"), std::string::npos) << p.warnings[0];
    EXPECT_NE(p.warnings[0].find("[alias(\"hp\")]"), std::string::npos) << p.warnings[0];

    nf[0].alias = "hp";
    p = hot::build_plan(o, n);
    EXPECT_EQ(p.steps[0].action, hot::FieldAction::Move);
    EXPECT_TRUE(p.warnings.empty());
    EXPECT_TRUE(p.removed.empty());
}

// ---- a struct stored inline (design 5.3): the migration recurses into it
struct InnerOld
{
    float hp;
    float armor;
};
struct InnerNew
{
    float mana;
    float hp;
    int armor; // the type changes
};
hot::FieldInfo inner_old_fields[] = {
    mk<float>("hp", nullptr, offsetof(InnerOld, hp), nullptr),
    mk<float>("armor", nullptr, offsetof(InnerOld, armor), nullptr),
};
hot::StateLayout inner_old{"Stats", sizeof(InnerOld), alignof(InnerOld), 2, inner_old_fields};
hot::FieldInfo inner_new_fields[] = {
    mk<float>("mana", nullptr, offsetof(InnerNew, mana), [](void*, void* f) { new (f) float(8.0f); }),
    mk<float>("hp", nullptr, offsetof(InnerNew, hp), nullptr),
    mk<int>("armor", nullptr, offsetof(InnerNew, armor), nullptr),
};
hot::StateLayout inner_new{"Stats", sizeof(InnerNew), alignof(InnerNew), 3, inner_new_fields};

hot::FieldInfo with_sub(hot::FieldInfo f, const hot::StateLayout* sub)
{
    f.sub = sub;
    return f;
}

TEST(MigrationPlan, ValueFieldsOfTheSameTypeAreMigratedRecursively)
{
    hot::FieldInfo of[] = {with_sub(mk<InnerOld>("stats", nullptr, 0, nullptr), &inner_old)};
    hot::FieldInfo nf[] = {with_sub(mk<InnerNew>("stats", nullptr, 0, nullptr), &inner_new)};
    // The two layouts use the same type key: the plan nests.
    of[0].type_key = nf[0].type_key = "Stats";
    hot::StateLayout o{"Hero", sizeof(InnerOld), alignof(InnerOld), 1, of};
    hot::StateLayout n{"Hero", sizeof(InnerNew), alignof(InnerNew), 1, nf};
    const hot::MigrationPlan p = hot::build_plan(o, n);
    ASSERT_EQ(p.steps.size(), 1u);
    ASSERT_EQ(p.steps[0].action, hot::FieldAction::Nested);
    ASSERT_NE(p.steps[0].nested, nullptr);
    const hot::MigrationPlan& sub = *p.steps[0].nested;
    ASSERT_EQ(sub.steps.size(), 3u);
    EXPECT_EQ(sub.steps[0].action, hot::FieldAction::Init);
    EXPECT_EQ(sub.steps[1].action, hot::FieldAction::Move);
    EXPECT_EQ(sub.steps[2].action, hot::FieldAction::Init); // the type changed
    ASSERT_EQ(p.warnings.size(), 1u);
    EXPECT_NE(p.warnings[0].find("Hero.stats: "), std::string::npos) << p.warnings[0];
    EXPECT_NE(p.warnings[0].find("armor"), std::string::npos) << p.warnings[0];
}

TEST(MigrationPlan, ValueFieldWithoutLayoutIsAPlainMove)
{
    hot::FieldInfo of[] = {mk<InnerOld>("stats", nullptr, 0, nullptr)};
    hot::FieldInfo nf[] = {mk<InnerOld>("stats", nullptr, 0, nullptr)};
    hot::StateLayout o{"Hero", sizeof(InnerOld), alignof(InnerOld), 1, of};
    hot::StateLayout n{"Hero", sizeof(InnerOld), alignof(InnerOld), 1, nf};
    EXPECT_EQ(hot::build_plan(o, n).steps[0].action, hot::FieldAction::Move);
}

// A field type without a copy constructor (std::atomic) has a move_field that compiles. The flag
// kNotMovable tells the migration to run the initializer. A move-only type moves.
TEST(MigrationPlan, FieldWithoutCopyIsFlaggedAndAMoveOnlyFieldMoves)
{
    static_assert(hot::field_flags<std::atomic<int>>() & hot::kNotMovable);
    static_assert(!(hot::field_flags<std::unique_ptr<int>>() & hot::kNotMovable));
    static_assert(!(hot::field_flags<std::string>() & hot::kNotMovable));
    alignas(std::atomic<int>) char to_atomic[sizeof(std::atomic<int>)];
    std::atomic<int> from_atomic{4};
    hot::move_field<std::atomic<int>>(to_atomic, &from_atomic); // no copy, no move: nothing happens
    alignas(std::unique_ptr<int>) char to_ptr[sizeof(std::unique_ptr<int>)];
    std::unique_ptr<int> from_ptr = std::make_unique<int>(9);
    hot::move_field<std::unique_ptr<int>>(to_ptr, &from_ptr);
    auto* moved = reinterpret_cast<std::unique_ptr<int>*>(to_ptr);
    ASSERT_NE(moved->get(), nullptr);
    EXPECT_EQ(**moved, 9);
    moved->~unique_ptr();
}

// A container of a move-only type reports a copy constructor, but the copy does not compile.
// The deep trait looks at the element types, so such a field moves.
TEST(MigrationPlan, ContainerOfMoveOnlyElementsMovesAndDoesNotCopy)
{
    using Vec = std::vector<std::unique_ptr<int>>;
    using Map = std::map<int, std::unique_ptr<int>>;
    using Pair = std::pair<int, std::unique_ptr<int>>;
    using Opt = std::optional<std::unique_ptr<int>>;
    static_assert(std::is_copy_constructible_v<Vec>);
    static_assert(!hot::deep_copyable_v<Vec>);
    static_assert(!hot::deep_copyable_v<Map>);
    static_assert(!hot::deep_copyable_v<Pair>);
    static_assert(!hot::deep_copyable_v<Opt>);
    static_assert(!hot::deep_copyable_v<std::vector<Vec>>);
    static_assert(!hot::deep_copyable_v<std::tuple<int, Vec>>);
    static_assert(hot::deep_copyable_v<std::vector<std::string>>);
    static_assert(hot::deep_copyable_v<std::map<int, std::pair<std::string, int>>>);
    static_assert(hot::deep_copyable_v<std::optional<std::shared_ptr<int>>>);
    static_assert(hot::deep_copyable_v<std::tuple<int, std::string>>);
    static_assert(!hot::deep_copyable_v<std::variant<int, Vec>>);
    static_assert(!hot::deep_copyable_v<std::variant<std::monostate, std::vector<Vec>>>);
    static_assert(hot::deep_copyable_v<std::variant<int, std::string, std::vector<std::string>>>);
    static_assert(!(hot::field_flags<Vec>() & hot::kNotMovable));
    static_assert(!(hot::field_flags<Map>() & hot::kNotMovable));
    static_assert(!hot::deep_copyable_v<std::vector<std::atomic<int>>>);

    alignas(Vec) char to_vec[sizeof(Vec)];
    Vec from_vec;
    from_vec.push_back(std::make_unique<int>(5));
    from_vec.push_back(std::make_unique<int>(6));
    hot::move_field<Vec>(to_vec, &from_vec);
    auto* moved = reinterpret_cast<Vec*>(to_vec);
    ASSERT_EQ(moved->size(), 2u);
    EXPECT_EQ(*(*moved)[1], 6);
    moved->~Vec();

    alignas(Map) char to_map[sizeof(Map)];
    Map from_map;
    from_map[3] = std::make_unique<int>(7);
    hot::move_field<Map>(to_map, &from_map);
    auto* moved_map = reinterpret_cast<Map*>(to_map);
    ASSERT_EQ(moved_map->size(), 1u);
    EXPECT_EQ(*(*moved_map)[3], 7);
    moved_map->~Map();

    // A copyable container still copies: the source keeps its elements.
    alignas(std::vector<std::string>) char to_str[sizeof(std::vector<std::string>)];
    std::vector<std::string> from_str = {"a", "b"};
    hot::move_field<std::vector<std::string>>(to_str, &from_str);
    EXPECT_EQ(from_str.size(), 2u);
    reinterpret_cast<std::vector<std::string>*>(to_str)->~vector();
}

TEST(MigrationPlan, MigrateBlockRunsTheInitializerForAFieldThatCannotMove)
{
    static bool move_ran = false;
    move_ran = false;
    hot::FieldInfo of[] = {mk<int>("n", nullptr, 0, nullptr)};
    hot::FieldInfo nf[] = {mk<int>("n", nullptr, 0, [](void*, void* f) { new (f) int(77); })};
    nf[0].flags = static_cast<uint8_t>(nf[0].flags | hot::kNotMovable);
    nf[0].move = [](void*, const void*) { move_ran = true; };
    hot::StateLayout o{"Counter", sizeof(int), alignof(int), 1, of};
    hot::StateLayout n{"Counter", sizeof(int), alignof(int), 1, nf};
    const hot::MigrationPlan plan = hot::build_plan(o, n);
    ASSERT_EQ(plan.steps[0].action, hot::FieldAction::Move); // the plan still pairs the field
    int from = 5;
    int to = 0;
    hot::migrate_block(o, n, plan, reinterpret_cast<char*>(&from), reinterpret_cast<char*>(&to));
    EXPECT_FALSE(move_ran);
    EXPECT_EQ(to, 77); // the initializer ran: the old value is not carried
}

TEST(MigrationPlan, PlainRemoveAndPlainAddGiveNoWarning)
{
    hot::FieldInfo of[] = {mk<float>("hp", nullptr, 0, nullptr)};
    hot::FieldInfo nf[] = {mk<int>("count", nullptr, 0, nullptr)}; // another type: not a rename
    hot::StateLayout o{"Player", 4, 4, 1, of};
    hot::StateLayout n{"Player", 4, 4, 1, nf};
    EXPECT_TRUE(hot::build_plan(o, n).warnings.empty());
}

TEST(Migration, ConvertDefaultRenameAndOnReloadHook)
{
    Scenario s;
    ASSERT_NO_FATAL_FAILURE(s.start());
    hot::ShellBase* before = s.obj;
    fields2[1].convert = [](void* dst, const void* src)
    { new (dst) float(convert_level(*static_cast<const int*>(src))); };
    fields2[1].convert_from = hot::type_key<int>();
    record2.on_reload = on_reload;

    std::string error;
    ASSERT_RELOAD_OK(s.loader.request_reload(module_of(register2, "gen2")));
    testing::internal::CaptureStderr();
    ASSERT_RELOAD_OK(s.loader.commit_pending());
    const std::string log = testing::internal::GetCapturedStderr();

    EXPECT_EQ(static_cast<hot::ShellBase*>(s.obj), before);
    EXPECT_EQ(*hot::field_ptr<float>(s.obj, "health"), 120.0f); // alias: moved
    EXPECT_EQ(*hot::field_ptr<float>(s.obj, "level"), 30.0f); // convert hook
    EXPECT_EQ(*hot::field_ptr<float>(s.obj, "tag"), 2.5f); // no hook: default
    EXPECT_EQ(*hot::field_ptr<int>(s.obj, "extra"), 7); // new field
    // The rename without alias: the value 33 is lost. The hook added 1 to the default.
    EXPECT_EQ(*hot::field_ptr<float>(s.obj, "energy"), 6.0f);

    EXPECT_NE(log.find("WARNING: State2: field tag changed its type"), std::string::npos) << log;
    EXPECT_NE(log.find("field mana is removed and field energy is added"), std::string::npos) << log;
    EXPECT_EQ(log.find("field level changed"), std::string::npos) << log;

    EXPECT_EQ(g_hook.calls, 1);
    EXPECT_TRUE(g_hook.has_hp); // also a field that the migration moved
    EXPECT_TRUE(g_hook.has_mana);
    EXPECT_FALSE(g_hook.has_nope);
    EXPECT_TRUE(g_hook.hp_is_float);
    EXPECT_FALSE(g_hook.hp_is_int);
    EXPECT_EQ(g_hook.old_hp, 120.0f);
    EXPECT_EQ(g_hook.old_level, 3);
    EXPECT_TRUE(g_hook.get_wrong_type_threw);
    EXPECT_TRUE(g_hook.get_missing_threw);
    EXPECT_FALSE(hot::restart_requested());
}

TEST(Migration, ATypeChangeWithoutAnyHookStillCommits)
{
    Scenario s;
    ASSERT_NO_FATAL_FAILURE(s.start());
    std::string error;
    ASSERT_RELOAD_OK(s.loader.request_reload(module_of(register2, "gen2")));
    testing::internal::CaptureStderr();
    ASSERT_RELOAD_OK(s.loader.commit_pending());
    const std::string log = testing::internal::GetCapturedStderr();
    EXPECT_NE(log.find("field level changed its type"), std::string::npos) << log;
    EXPECT_EQ(*hot::field_ptr<float>(s.obj, "level"), 1.5f);
    EXPECT_EQ(g_hook.calls, 0);
    EXPECT_EQ(hot::active_generation, 2u);
}

TEST(Migration, AThrowingOnReloadIsLoggedAndTheObjectStaysValid)
{
    Scenario s;
    ASSERT_NO_FATAL_FAILURE(s.start());
    record2.on_reload = on_reload;
    g_hook_throws = true;
    std::string error;
    ASSERT_RELOAD_OK(s.loader.request_reload(module_of(register2, "gen2")));
    testing::internal::CaptureStderr();
    ASSERT_RELOAD_OK(s.loader.commit_pending());
    const std::string log = testing::internal::GetCapturedStderr();
    EXPECT_NE(log.find("ERROR: onReload of Fake threw: boom"), std::string::npos) << log;
    EXPECT_EQ(g_hook.calls, 1);
    EXPECT_EQ(hot::active_generation, 2u);
    EXPECT_EQ(*hot::field_ptr<float>(s.obj, "health"), 120.0f);
    EXPECT_EQ(*hot::field_ptr<int>(s.obj, "extra"), 7);
    EXPECT_FALSE(hot::restart_requested());
}

TEST(Migration, ABaseClassChangeIsStillRejected)
{
    Scenario s;
    ASSERT_NO_FATAL_FAILURE(s.start());
    const char* saved = record2.base_name;
    record2.base_name = "Other";
    const hot::ReloadResult rejected = s.loader.request_reload(module_of(register2, "gen2"));
    const std::string& error = rejected.error;
    EXPECT_FALSE(rejected);
    EXPECT_EQ(rejected.status, hot::ReloadStatus::FullReloadRequired);
    EXPECT_EQ(rejected.reason, hot::ReloadReason::NativeBaseChanged);
    EXPECT_NE(error.find("base class changed"), std::string::npos) << error;
    record2.base_name = saved;
    EXPECT_EQ(hot::active_generation, 1u);
}

// ---- 6.6: a native move constructor that throws during the migration is fatal.
struct Thrower
{
    int v;
    explicit Thrower(int x)
        : v(x)
    {
    }
    Thrower(const Thrower& o)
        : v(o.v)
    {
        if (armed)
            throw std::runtime_error("native move failed");
    }
    static inline bool armed = false;
};
struct TState
{
    Thrower t;
};
hot::FieldInfo tfields[] = {
    mk<Thrower>("t", nullptr, offsetof(TState, t), [](void*, void* f) { new (f) Thrower(1); }),
};
hot::StateLayout tlayout{"TState", sizeof(TState), alignof(TState), 1, tfields};
hot::ClassRecord trecord1{"Thrown", 1, slots, 1, &tlayout, nullptr, "Base", kHash};
hot::ClassRecord trecord2{"Thrown", 2, slots, 1, &tlayout, nullptr, "Base", kHash};
hot::Instance tcreate()
{
    auto* s = new Fake;
    hot::init_state(s, &trecord1);
    return {s, s};
}
void tregister1(hot::Registry& reg)
{
    reg.add(&trecord1, &tcreate);
}
void tregister2(hot::Registry& reg)
{
    reg.add(&trecord2, &tcreate);
}

std::string g_restart_reason;
void test_handler(const char* reason)
{
    g_restart_reason = reason;
}

TEST(Migration, AThrowingNativeMoveRequestsARestart)
{
    trecord1 = hot::ClassRecord{"Thrown", 1, slots, 1, &tlayout, nullptr, "Base", kHash};
    hot::Loader loader;
    std::string error;
    ASSERT_RELOAD_OK(loader.load(module_of(tregister1, "t1")));
    Fake* obj = static_cast<Fake*>(loader.find("Thrown")->create().shell);
    hot::field_ptr<Thrower>(obj, "t")->v = 42;

    ASSERT_RELOAD_OK(loader.request_reload(module_of(tregister2, "t2")));
    g_restart_reason.clear();
    hot::clear_restart_request();
    hot::set_restart_handler(test_handler);
    Thrower::armed = true;
    testing::internal::CaptureStderr();
    const hot::ReloadResult committed = loader.commit_pending();
    EXPECT_FALSE(committed);
    EXPECT_EQ(committed.reason, hot::ReloadReason::MigrationFailed);
    const std::string log = testing::internal::GetCapturedStderr();
    Thrower::armed = false;
    hot::set_restart_handler(nullptr);

    EXPECT_TRUE(hot::restart_requested());
    EXPECT_NE(g_restart_reason.find("FATAL"), std::string::npos) << g_restart_reason;
    EXPECT_NE(committed.error.find("restart"), std::string::npos) << committed.error;
    EXPECT_NE(log.find("FATAL: restart requested"), std::string::npos) << log;
    // The old block stays valid and the generation does not change.
    EXPECT_EQ(hot::field_ptr<Thrower>(obj, "t")->v, 42);
    EXPECT_EQ(hot::active_generation, 1u);
    EXPECT_FALSE(loader.reload_pending());
    hot::clear_restart_request();
    delete obj;
}

// ---- a native container of a struct (design 5.3, S17)
// The type key has the hash of the layout of the value class. The same hash moves the container.
// Another hash resets it to the default value with a warning.
TEST(ContainerOfValueType, SameLayoutMovesOtherLayoutResets)
{
    using V = std::vector<int>;
    const char* k1 = hot::layout_keyed(typeid(V).name(), "aaaa");
    const char* k1b = hot::layout_keyed(typeid(V).name(), "aaaa");
    const char* k2 = hot::layout_keyed(typeid(V).name(), "bbbb");
    EXPECT_EQ(k1, k1b); // one text for each key
    EXPECT_TRUE(hot::type_key_matches(k1, typeid(V).name()));
    EXPECT_FALSE(hot::type_key_matches(k1, typeid(int).name()));
    auto row = [](const char* key)
    {
        return hot::FieldInfo{"items", nullptr, key, 0, sizeof(V), alignof(V), hot::field_flags<V>(), nullptr,
            hot::move_field<V>, hot::field_destroyer<V>()};
    };
    const hot::FieldInfo f1[] = {row(k1)}, f1b[] = {row(k1b)}, f2[] = {row(k2)};
    const hot::StateLayout l1{"BagState_g1", sizeof(V), alignof(V), 1, f1};
    const hot::StateLayout l1b{"BagState_g2", sizeof(V), alignof(V), 1, f1b};
    const hot::StateLayout l2{"BagState_g3", sizeof(V), alignof(V), 1, f2};
    const hot::MigrationPlan same = hot::build_plan(l1, l1b);
    EXPECT_EQ(same.steps[0].action, hot::FieldAction::Move);
    EXPECT_TRUE(same.warnings.empty());
    const hot::MigrationPlan other = hot::build_plan(l1, l2);
    EXPECT_EQ(other.steps[0].action, hot::FieldAction::Init);
    ASSERT_EQ(other.warnings.size(), 1u);
    EXPECT_NE(other.warnings[0].find("container resets"), std::string::npos) << other.warnings[0];
    EXPECT_NE(other.warnings[0].find("std::vector<int"), std::string::npos) << other.warnings[0];
}

// ---- code pointers in a state field (design 4.9, S17)
struct PlainData
{
    int a;
    float b;
};
struct Obj
{
    int f(int x) { return x; }
};
TEST(MayHoldCode, FunctionPointersAndStdFunctionHoldCode)
{
    EXPECT_TRUE(hot::field_flags<int (*)(int)>() & hot::kMayHoldCode);
    EXPECT_TRUE(hot::field_flags<void (*)() noexcept>() & hot::kMayHoldCode);
    EXPECT_TRUE(hot::field_flags<int (Obj::*)(int)>() & hot::kMayHoldCode);
    EXPECT_TRUE((hot::field_flags<std::array<void (*)(), 2>>() & hot::kMayHoldCode));
    EXPECT_TRUE(hot::field_flags<std::vector<int (*)(int)>>() & hot::kMayHoldCode);
    EXPECT_TRUE(hot::field_flags<std::function<int(int)>>() & hot::kMayHoldCode);
    EXPECT_TRUE(hot::field_flags<void (*const)()>() & hot::kMayHoldCode);
    // The trivially copyable function pointer keeps its copy flag.
    EXPECT_TRUE(hot::field_flags<int (*)(int)>() & hot::kTriviallyCopyable);
    EXPECT_FALSE(hot::field_flags<int>() & hot::kMayHoldCode);
    EXPECT_FALSE(hot::field_flags<int*>() & hot::kMayHoldCode);
    EXPECT_FALSE(hot::field_flags<PlainData>() & hot::kMayHoldCode);
    EXPECT_FALSE(hot::field_flags<int Obj::*>() & hot::kMayHoldCode); // a data member pointer
}

// ---- the owner slot (has_owner)
struct OwnedState
{
    hot::ShellBase* owner;
    int value;
};
hot::FieldInfo owned_fields[] = {
    mk<int>("value", nullptr, offsetof(OwnedState, value), [](void*, void* f) { new (f) int(7); }),
};
hot::StateLayout owned_layout{"Owned", sizeof(OwnedState), alignof(OwnedState), 1, owned_fields, true};

struct PlainState
{
    int first;
};
hot::FieldInfo plain_fields[] = {
    mk<int>("first", nullptr, offsetof(PlainState, first), [](void*, void* f) { new (f) int(42); }),
};
hot::StateLayout plain_layout{"Plain", sizeof(PlainState), alignof(PlainState), 1, plain_fields};

TEST(OwnerSlot, InitStateWritesTheOwnerFirstSlot)
{
    hot::ClassRecord rec{};
    rec.name = "Owned";
    rec.layout = &owned_layout;
    auto obj = std::make_unique<Fake>();
    hot::init_state(obj.get(), &rec);
    ASSERT_NE(obj->state, nullptr);
    EXPECT_EQ(*reinterpret_cast<hot::ShellBase**>(obj->state), obj.get());
    EXPECT_EQ(static_cast<OwnedState*>(obj->state)->value, 7);
    hot::destroy_state(obj.get());
    EXPECT_EQ(obj->state, nullptr);
}

TEST(OwnerSlot, InitStateKeepsTheFirstFieldWithoutAnOwner)
{
    hot::ClassRecord rec{};
    rec.name = "Plain";
    rec.layout = &plain_layout;
    auto obj = std::make_unique<Fake>();
    hot::init_state(obj.get(), &rec);
    ASSERT_NE(obj->state, nullptr);
    EXPECT_EQ(static_cast<PlainState*>(obj->state)->first, 42);
    hot::destroy_state(obj.get());
}

} // namespace
