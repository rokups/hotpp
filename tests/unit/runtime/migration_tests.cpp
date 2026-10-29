// Unit tests of the migration (design 4.7 and 6.6) with hand-written records and no backend.
// The tests cover the plan, the convert hook, the onReload hook with OldState,
// the warnings, the rejections that stay and the restart request.
#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "spp/runtime.h"

namespace {

// ---- the fake script class "Fake": one generation 1 and one generation 2 in the same test file
struct Fake : spp::ShellBase {
    virtual ~Fake() { spp::destroy_state(this); }
};

struct State1 {
    float hp;
    int level;
    float mana;
    int tag;
};
struct State2 {
    float health;
    float level;
    float energy;
    float tag;
    int extra;
};

template <class T>
spp::FieldInfo mk(const char* name, const char* alias, uint32_t off, void (*init)(void*, void*))
{
    return {name,         alias, spp::type_key<T>(), off, sizeof(T), alignof(T), spp::field_flags<T>(),
            init,         spp::move_field<T>,        spp::field_destroyer<T>()};
}

spp::FieldInfo fields1[] = {
    mk<float>("hp", nullptr, offsetof(State1, hp), [](void*, void* f) { new (f) float(100.0f); }),
    mk<int>("level", nullptr, offsetof(State1, level), [](void*, void* f) { new (f) int(1); }),
    mk<float>("mana", nullptr, offsetof(State1, mana), [](void*, void* f) { new (f) float(10.0f); }),
    mk<int>("tag", nullptr, offsetof(State1, tag), [](void*, void* f) { new (f) int(5); }),
};
spp::StateLayout layout1{"State1", sizeof(State1), alignof(State1), 4, fields1};

float convert_level(int old) { return static_cast<float>(old) * 10.0f; }

spp::FieldInfo fields2[] = {
    mk<float>("health", "hp", offsetof(State2, health), [](void*, void* f) { new (f) float(50.0f); }),
    mk<float>("level", nullptr, offsetof(State2, level), [](void*, void* f) { new (f) float(1.5f); }),
    mk<float>("energy", nullptr, offsetof(State2, energy), [](void*, void* f) { new (f) float(5.0f); }),
    mk<float>("tag", nullptr, offsetof(State2, tag), [](void*, void* f) { new (f) float(2.5f); }),
    mk<int>("extra", nullptr, offsetof(State2, extra), [](void*, void* f) { new (f) int(7); }),
};
spp::StateLayout layout2{"State2", sizeof(State2), alignof(State2), 5, fields2};

void* slots[1] = {nullptr};
constexpr uint64_t kHash = spp::shell_layout_hash("Base", sizeof(Fake), alignof(Fake), 0);

spp::ClassRecord record1{"Fake", 1, slots, 1, &layout1, nullptr, "Base", kHash};
spp::ClassRecord record2{"Fake", 2, slots, 1, &layout2, nullptr, "Base", kHash};

spp::Instance create1()
{
    auto* s = new Fake;
    spp::init_state(s, &record1);
    return {s, s};
}
spp::Instance create2()
{
    auto* s = new Fake;
    spp::init_state(s, &record2);
    return {s, s};
}
void register1(spp::Registry& reg) { reg.add(&record1, &create1); }
void register2(spp::Registry& reg) { reg.add(&record2, &create2); }

// What the onReload hook saw.
struct HookLog {
    int calls = 0;
    float old_hp = 0;
    bool has_hp = false, has_mana = false, has_nope = false, hp_is_float = false, hp_is_int = true;
    bool get_wrong_type_threw = false, get_missing_threw = false;
    int old_level = 0;
};
HookLog g_hook;
bool g_hook_throws = false;

void on_reload(spp::ShellBase* self, const spp::OldState& old)
{
    ++g_hook.calls;
    g_hook.has_hp = old.has("hp");
    g_hook.has_mana = old.has("mana");
    g_hook.has_nope = old.has("nope");
    g_hook.hp_is_float = old.has<float>("hp");
    g_hook.hp_is_int = old.has<int>("hp");
    g_hook.old_hp = old.get<float>("hp");
    g_hook.old_level = old.get<int>("level");
    try {
        old.get<int>("hp");
    } catch (const std::runtime_error&) {
        g_hook.get_wrong_type_threw = true;
    }
    try {
        old.get<int>("nope");
    } catch (const std::runtime_error&) {
        g_hook.get_missing_threw = true;
    }
    // The structural migration is done: the new fields are valid.
    *spp::field_ptr<float>(self, "energy") += 1.0f;
    if (g_hook_throws) throw std::runtime_error("boom");
}

spp::Module module_of(spp::Module::RegisterFn fn, const char* name)
{
    spp::Module m;
    m.register_fn = fn;
    m.origin = name;
    return m;
}

// The loader is the first member, so it is the last to go.
struct Scenario {
    spp::Loader loader;
    Fake* obj = nullptr;
    Scenario()
    {
        // A commit changes the first record of a class. Each test starts from a new one.
        record1 = spp::ClassRecord{"Fake", 1, slots, 1, &layout1, nullptr, "Base", kHash};
        g_hook = HookLog{};
        g_hook_throws = false;
        record2.on_reload = nullptr;
        fields2[1].convert = nullptr;
        fields2[1].convert_from = nullptr;
    }
    void start()
    {
        std::string error;
        ASSERT_TRUE(loader.load(module_of(register1, "gen1"), error)) << error;
        spp::Instance inst = loader.find("Fake")->create();
        obj = static_cast<Fake*>(inst.shell);
        *spp::field_ptr<float>(obj, "hp") = 120.0f;
        *spp::field_ptr<int>(obj, "level") = 3;
        *spp::field_ptr<float>(obj, "mana") = 33.0f;
        *spp::field_ptr<int>(obj, "tag") = 9;
    }
    ~Scenario()
    {
        delete obj;
        record2.on_reload = nullptr;
    }
};

TEST(MigrationPlan, ConvertHookIsChosenOnlyForTheMatchingOldType)
{
    spp::FieldInfo of[] = {mk<int>("level", nullptr, 0, nullptr)};
    spp::FieldInfo nf[] = {mk<float>("level", nullptr, 0, nullptr)};
    spp::StateLayout o{"S", 4, 4, 1, of};
    spp::StateLayout n{"S", 4, 4, 1, nf};
    nf[0].convert = [](void* dst, const void* src) {
        new (dst) float(static_cast<float>(*static_cast<const int*>(src)));
    };
    nf[0].convert_from = spp::type_key<int>();
    spp::MigrationPlan p = spp::build_plan(o, n);
    EXPECT_EQ(p.steps[0].action, spp::FieldAction::Convert);
    EXPECT_TRUE(p.warnings.empty());
    EXPECT_EQ(p.changed, std::vector<uint32_t>{0});

    nf[0].convert_from = spp::type_key<double>();  // the hook takes another old type
    p = spp::build_plan(o, n);
    EXPECT_EQ(p.steps[0].action, spp::FieldAction::Init);
    EXPECT_EQ(p.warnings.size(), 1u);
}

TEST(MigrationPlan, RenameWithoutAliasWarnsAndAliasDoesNot)
{
    spp::FieldInfo of[] = {mk<float>("hp", nullptr, 0, nullptr)};
    spp::FieldInfo nf[] = {mk<float>("health", nullptr, 0, nullptr)};
    spp::StateLayout o{"Player", 4, 4, 1, of};
    spp::StateLayout n{"Player", 4, 4, 1, nf};
    spp::MigrationPlan p = spp::build_plan(o, n);
    EXPECT_EQ(p.steps[0].action, spp::FieldAction::Init);
    EXPECT_EQ(p.removed, std::vector<uint32_t>{0});
    ASSERT_EQ(p.warnings.size(), 1u);
    EXPECT_NE(p.warnings[0].find("hp is removed and field health is added"), std::string::npos)
        << p.warnings[0];
    EXPECT_NE(p.warnings[0].find("[alias(\"hp\")]"), std::string::npos) << p.warnings[0];

    nf[0].alias = "hp";
    p = spp::build_plan(o, n);
    EXPECT_EQ(p.steps[0].action, spp::FieldAction::Move);
    EXPECT_TRUE(p.warnings.empty());
    EXPECT_TRUE(p.removed.empty());
}

// ---- a [value] script class stored inline (design 5.3): the migration recurses into it
struct InnerOld {
    float hp;
    float armor;
};
struct InnerNew {
    float mana;
    float hp;
    int armor;  // the type changes
};
spp::FieldInfo inner_old_fields[] = {
    mk<float>("hp", nullptr, offsetof(InnerOld, hp), nullptr),
    mk<float>("armor", nullptr, offsetof(InnerOld, armor), nullptr),
};
spp::StateLayout inner_old{"Stats", sizeof(InnerOld), alignof(InnerOld), 2, inner_old_fields};
spp::FieldInfo inner_new_fields[] = {
    mk<float>("mana", nullptr, offsetof(InnerNew, mana), [](void*, void* f) { new (f) float(8.0f); }),
    mk<float>("hp", nullptr, offsetof(InnerNew, hp), nullptr),
    mk<int>("armor", nullptr, offsetof(InnerNew, armor), nullptr),
};
spp::StateLayout inner_new{"Stats", sizeof(InnerNew), alignof(InnerNew), 3, inner_new_fields};

spp::FieldInfo with_sub(spp::FieldInfo f, const spp::StateLayout* sub)
{
    f.sub = sub;
    return f;
}

TEST(MigrationPlan, ValueFieldsOfTheSameTypeAreMigratedRecursively)
{
    spp::FieldInfo of[] = {with_sub(mk<InnerOld>("stats", nullptr, 0, nullptr), &inner_old)};
    spp::FieldInfo nf[] = {with_sub(mk<InnerNew>("stats", nullptr, 0, nullptr), &inner_new)};
    // The two layouts use the same type key: the plan nests.
    of[0].type_key = nf[0].type_key = "Stats";
    spp::StateLayout o{"Hero", sizeof(InnerOld), alignof(InnerOld), 1, of};
    spp::StateLayout n{"Hero", sizeof(InnerNew), alignof(InnerNew), 1, nf};
    const spp::MigrationPlan p = spp::build_plan(o, n);
    ASSERT_EQ(p.steps.size(), 1u);
    ASSERT_EQ(p.steps[0].action, spp::FieldAction::Nested);
    ASSERT_NE(p.steps[0].nested, nullptr);
    const spp::MigrationPlan& sub = *p.steps[0].nested;
    ASSERT_EQ(sub.steps.size(), 3u);
    EXPECT_EQ(sub.steps[0].action, spp::FieldAction::Init);
    EXPECT_EQ(sub.steps[1].action, spp::FieldAction::Move);
    EXPECT_EQ(sub.steps[2].action, spp::FieldAction::Init);  // the type changed
    ASSERT_EQ(p.warnings.size(), 1u);
    EXPECT_NE(p.warnings[0].find("Hero.stats: "), std::string::npos) << p.warnings[0];
    EXPECT_NE(p.warnings[0].find("armor"), std::string::npos) << p.warnings[0];
}

TEST(MigrationPlan, ValueFieldWithoutLayoutIsAPlainMove)
{
    spp::FieldInfo of[] = {mk<InnerOld>("stats", nullptr, 0, nullptr)};
    spp::FieldInfo nf[] = {mk<InnerOld>("stats", nullptr, 0, nullptr)};
    spp::StateLayout o{"Hero", sizeof(InnerOld), alignof(InnerOld), 1, of};
    spp::StateLayout n{"Hero", sizeof(InnerOld), alignof(InnerOld), 1, nf};
    EXPECT_EQ(spp::build_plan(o, n).steps[0].action, spp::FieldAction::Move);
}

TEST(MigrationPlan, PlainRemoveAndPlainAddGiveNoWarning)
{
    spp::FieldInfo of[] = {mk<float>("hp", nullptr, 0, nullptr)};
    spp::FieldInfo nf[] = {mk<int>("count", nullptr, 0, nullptr)};  // another type: not a rename
    spp::StateLayout o{"Player", 4, 4, 1, of};
    spp::StateLayout n{"Player", 4, 4, 1, nf};
    EXPECT_TRUE(spp::build_plan(o, n).warnings.empty());
}

TEST(Migration, ConvertDefaultRenameAndOnReloadHook)
{
    Scenario s;
    ASSERT_NO_FATAL_FAILURE(s.start());
    spp::ShellBase* before = s.obj;
    fields2[1].convert = [](void* dst, const void* src) {
        new (dst) float(convert_level(*static_cast<const int*>(src)));
    };
    fields2[1].convert_from = spp::type_key<int>();
    record2.on_reload = on_reload;

    std::string error;
    ASSERT_TRUE(s.loader.request_reload(module_of(register2, "gen2"), error)) << error;
    testing::internal::CaptureStderr();
    ASSERT_TRUE(s.loader.commit_pending(error)) << error;
    const std::string log = testing::internal::GetCapturedStderr();

    EXPECT_EQ(static_cast<spp::ShellBase*>(s.obj), before);
    EXPECT_EQ(*spp::field_ptr<float>(s.obj, "health"), 120.0f);  // alias: moved
    EXPECT_EQ(*spp::field_ptr<float>(s.obj, "level"), 30.0f);    // convert hook
    EXPECT_EQ(*spp::field_ptr<float>(s.obj, "tag"), 2.5f);       // no hook: default
    EXPECT_EQ(*spp::field_ptr<int>(s.obj, "extra"), 7);          // new field
    // The rename without alias: the value 33 is lost. The hook added 1 to the default.
    EXPECT_EQ(*spp::field_ptr<float>(s.obj, "energy"), 6.0f);

    EXPECT_NE(log.find("WARNING: State2: field tag changed its type"), std::string::npos) << log;
    EXPECT_NE(log.find("field mana is removed and field energy is added"), std::string::npos) << log;
    EXPECT_EQ(log.find("field level changed"), std::string::npos) << log;

    EXPECT_EQ(g_hook.calls, 1);
    EXPECT_TRUE(g_hook.has_hp);  // also a field that the migration moved
    EXPECT_TRUE(g_hook.has_mana);
    EXPECT_FALSE(g_hook.has_nope);
    EXPECT_TRUE(g_hook.hp_is_float);
    EXPECT_FALSE(g_hook.hp_is_int);
    EXPECT_EQ(g_hook.old_hp, 120.0f);
    EXPECT_EQ(g_hook.old_level, 3);
    EXPECT_TRUE(g_hook.get_wrong_type_threw);
    EXPECT_TRUE(g_hook.get_missing_threw);
    EXPECT_FALSE(spp::restart_requested());
}

TEST(Migration, ATypeChangeWithoutAnyHookStillCommits)
{
    Scenario s;
    ASSERT_NO_FATAL_FAILURE(s.start());
    std::string error;
    ASSERT_TRUE(s.loader.request_reload(module_of(register2, "gen2"), error)) << error;
    testing::internal::CaptureStderr();
    ASSERT_TRUE(s.loader.commit_pending(error)) << error;
    const std::string log = testing::internal::GetCapturedStderr();
    EXPECT_NE(log.find("field level changed its type"), std::string::npos) << log;
    EXPECT_EQ(*spp::field_ptr<float>(s.obj, "level"), 1.5f);
    EXPECT_EQ(g_hook.calls, 0);
    EXPECT_EQ(spp::active_generation, 2u);
}

TEST(Migration, AThrowingOnReloadIsLoggedAndTheObjectStaysValid)
{
    Scenario s;
    ASSERT_NO_FATAL_FAILURE(s.start());
    record2.on_reload = on_reload;
    g_hook_throws = true;
    std::string error;
    ASSERT_TRUE(s.loader.request_reload(module_of(register2, "gen2"), error)) << error;
    testing::internal::CaptureStderr();
    ASSERT_TRUE(s.loader.commit_pending(error)) << error;
    const std::string log = testing::internal::GetCapturedStderr();
    EXPECT_NE(log.find("ERROR: onReload of Fake threw: boom"), std::string::npos) << log;
    EXPECT_EQ(g_hook.calls, 1);
    EXPECT_EQ(spp::active_generation, 2u);
    EXPECT_EQ(*spp::field_ptr<float>(s.obj, "health"), 120.0f);
    EXPECT_EQ(*spp::field_ptr<int>(s.obj, "extra"), 7);
    EXPECT_FALSE(spp::restart_requested());
}

TEST(Migration, ABaseClassChangeIsStillRejected)
{
    Scenario s;
    ASSERT_NO_FATAL_FAILURE(s.start());
    const char* saved = record2.base_name;
    record2.base_name = "Other";
    std::string error;
    EXPECT_FALSE(s.loader.request_reload(module_of(register2, "gen2"), error));
    EXPECT_NE(error.find("base class changed"), std::string::npos) << error;
    record2.base_name = saved;
    EXPECT_EQ(spp::active_generation, 1u);
}

// ---- 6.6: a native move constructor that throws during the migration is fatal.
struct Thrower {
    int v;
    explicit Thrower(int x) : v(x) {}
    Thrower(const Thrower& o) : v(o.v)
    {
        if (armed) throw std::runtime_error("native move failed");
    }
    static inline bool armed = false;
};
struct TState {
    Thrower t;
};
spp::FieldInfo tfields[] = {
    mk<Thrower>("t", nullptr, offsetof(TState, t), [](void*, void* f) { new (f) Thrower(1); }),
};
spp::StateLayout tlayout{"TState", sizeof(TState), alignof(TState), 1, tfields};
spp::ClassRecord trecord1{"Thrown", 1, slots, 1, &tlayout, nullptr, "Base", kHash};
spp::ClassRecord trecord2{"Thrown", 2, slots, 1, &tlayout, nullptr, "Base", kHash};
spp::Instance tcreate()
{
    auto* s = new Fake;
    spp::init_state(s, &trecord1);
    return {s, s};
}
void tregister1(spp::Registry& reg) { reg.add(&trecord1, &tcreate); }
void tregister2(spp::Registry& reg) { reg.add(&trecord2, &tcreate); }

std::string g_restart_reason;
void test_handler(const char* reason) { g_restart_reason = reason; }

TEST(Migration, AThrowingNativeMoveRequestsARestart)
{
    trecord1 = spp::ClassRecord{"Thrown", 1, slots, 1, &tlayout, nullptr, "Base", kHash};
    spp::Loader loader;
    std::string error;
    ASSERT_TRUE(loader.load(module_of(tregister1, "t1"), error)) << error;
    Fake* obj = static_cast<Fake*>(loader.find("Thrown")->create().shell);
    spp::field_ptr<Thrower>(obj, "t")->v = 42;

    ASSERT_TRUE(loader.request_reload(module_of(tregister2, "t2"), error)) << error;
    g_restart_reason.clear();
    spp::clear_restart_request();
    spp::set_restart_handler(test_handler);
    Thrower::armed = true;
    testing::internal::CaptureStderr();
    EXPECT_FALSE(loader.commit_pending(error));
    const std::string log = testing::internal::GetCapturedStderr();
    Thrower::armed = false;
    spp::set_restart_handler(nullptr);

    EXPECT_TRUE(spp::restart_requested());
    EXPECT_NE(g_restart_reason.find("FATAL"), std::string::npos) << g_restart_reason;
    EXPECT_NE(error.find("restart"), std::string::npos) << error;
    EXPECT_NE(log.find("FATAL: restart requested"), std::string::npos) << log;
    // The old block stays valid and the generation does not change.
    EXPECT_EQ(spp::field_ptr<Thrower>(obj, "t")->v, 42);
    EXPECT_EQ(spp::active_generation, 1u);
    EXPECT_FALSE(loader.reload_pending());
    spp::clear_restart_request();
    delete obj;
}

// ---- a native container of a [value] script class (design 5.3, S17)
// The type key has the hash of the layout of the value class. The same hash moves the container.
// Another hash resets it to the default value with a warning.
TEST(ContainerOfValueType, SameLayoutMovesOtherLayoutResets)
{
    using V = std::vector<int>;
    const char* k1 = spp::layout_keyed(typeid(V).name(), "aaaa");
    const char* k1b = spp::layout_keyed(typeid(V).name(), "aaaa");
    const char* k2 = spp::layout_keyed(typeid(V).name(), "bbbb");
    EXPECT_EQ(k1, k1b);  // one text for each key
    EXPECT_TRUE(spp::type_key_matches(k1, typeid(V).name()));
    EXPECT_FALSE(spp::type_key_matches(k1, typeid(int).name()));
    auto row = [](const char* key) {
        return spp::FieldInfo{"items", nullptr, key, 0, sizeof(V), alignof(V), spp::field_flags<V>(),
                              nullptr, spp::move_field<V>, spp::field_destroyer<V>()};
    };
    const spp::FieldInfo f1[] = {row(k1)}, f1b[] = {row(k1b)}, f2[] = {row(k2)};
    const spp::StateLayout l1{"BagState_g1", sizeof(V), alignof(V), 1, f1};
    const spp::StateLayout l1b{"BagState_g2", sizeof(V), alignof(V), 1, f1b};
    const spp::StateLayout l2{"BagState_g3", sizeof(V), alignof(V), 1, f2};
    const spp::MigrationPlan same = spp::build_plan(l1, l1b);
    EXPECT_EQ(same.steps[0].action, spp::FieldAction::Move);
    EXPECT_TRUE(same.warnings.empty());
    const spp::MigrationPlan other = spp::build_plan(l1, l2);
    EXPECT_EQ(other.steps[0].action, spp::FieldAction::Init);
    ASSERT_EQ(other.warnings.size(), 1u);
    EXPECT_NE(other.warnings[0].find("container resets"), std::string::npos) << other.warnings[0];
    EXPECT_NE(other.warnings[0].find("std::vector<int"), std::string::npos) << other.warnings[0];
}

// ---- code pointers in a state field (design 4.9, S17)
struct PlainData {
    int a;
    float b;
};
struct Obj {
    int f(int x) { return x; }
};
TEST(MayHoldCode, FunctionPointersAndStdFunctionHoldCode)
{
    EXPECT_TRUE(spp::field_flags<int (*)(int)>() & spp::kMayHoldCode);
    EXPECT_TRUE(spp::field_flags<void (*)() noexcept>() & spp::kMayHoldCode);
    EXPECT_TRUE(spp::field_flags<int (Obj::*)(int)>() & spp::kMayHoldCode);
    EXPECT_TRUE((spp::field_flags<std::array<void (*)(), 2>>() & spp::kMayHoldCode));
    EXPECT_TRUE(spp::field_flags<std::vector<int (*)(int)>>() & spp::kMayHoldCode);
    EXPECT_TRUE(spp::field_flags<std::function<int(int)>>() & spp::kMayHoldCode);
    EXPECT_TRUE(spp::field_flags<void (*const)()>() & spp::kMayHoldCode);
    // The trivially copyable function pointer keeps its copy flag.
    EXPECT_TRUE(spp::field_flags<int (*)(int)>() & spp::kTriviallyCopyable);
    EXPECT_FALSE(spp::field_flags<int>() & spp::kMayHoldCode);
    EXPECT_FALSE(spp::field_flags<int*>() & spp::kMayHoldCode);
    EXPECT_FALSE(spp::field_flags<PlainData>() & spp::kMayHoldCode);
    EXPECT_FALSE(spp::field_flags<int Obj::*>() & spp::kMayHoldCode);  // a data member pointer
}

// ---- the owner slot (has_owner)
struct OwnedState {
    spp::ShellBase* owner;
    int value;
};
spp::FieldInfo owned_fields[] = {
    mk<int>("value", nullptr, offsetof(OwnedState, value), [](void*, void* f) { new (f) int(7); }),
};
spp::StateLayout owned_layout{"Owned", sizeof(OwnedState), alignof(OwnedState), 1, owned_fields, true};

struct PlainState {
    int first;
};
spp::FieldInfo plain_fields[] = {
    mk<int>("first", nullptr, offsetof(PlainState, first), [](void*, void* f) { new (f) int(42); }),
};
spp::StateLayout plain_layout{"Plain", sizeof(PlainState), alignof(PlainState), 1, plain_fields};

TEST(OwnerSlot, InitStateWritesTheOwnerFirstSlot)
{
    spp::ClassRecord rec{};
    rec.name = "Owned";
    rec.layout = &owned_layout;
    auto obj = std::make_unique<Fake>();
    spp::init_state(obj.get(), &rec);
    ASSERT_NE(obj->state, nullptr);
    EXPECT_EQ(*reinterpret_cast<spp::ShellBase**>(obj->state), obj.get());
    EXPECT_EQ(static_cast<OwnedState*>(obj->state)->value, 7);
    spp::destroy_state(obj.get());
    EXPECT_EQ(obj->state, nullptr);
}

TEST(OwnerSlot, InitStateKeepsTheFirstFieldWithoutAnOwner)
{
    spp::ClassRecord rec{};
    rec.name = "Plain";
    rec.layout = &plain_layout;
    auto obj = std::make_unique<Fake>();
    spp::init_state(obj.get(), &rec);
    ASSERT_NE(obj->state, nullptr);
    EXPECT_EQ(static_cast<PlainState*>(obj->state)->first, 42);
    spp::destroy_state(obj.get());
}

}  // namespace
