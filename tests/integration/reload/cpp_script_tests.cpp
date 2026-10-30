// Reload tests of a plain C++ script (a ".cpp" script). The file extension selects the parser
// of sppc. The host passes the script as a ScriptSource like any other script. The tests use
// the backend that SPP_BACKEND selected.
#include "spp/backend.h"
#include "support/backend_fixture.h"
#include "support/reload_assert.h"
#include "support/script_files.h"
#include "support/test_support.h"

#include <gtest/gtest.h>

#include <functional>
#include <string>

namespace
{

using test_support::kDt;
using test_support::Session;
using test_support::source;

template <class T> T* fld(spp::ShellBase* shell, const char* name)
{
    return spp::field_ptr<T>(shell, name);
}

// The module state block of the loader. The test reads a variable by its name.
template <class T> T* modvar(Session& s, const char* name)
{
    const spp::ModuleStateInfo ms = s.loader.module_state();
    if (!ms.layout || !*ms.slot)
        return nullptr;
    for (uint32_t i = 0; i < ms.layout->field_count; ++i)
        if (std::string(ms.layout->fields[i].name) == name)
            return reinterpret_cast<T*>(static_cast<char*>(*ms.slot) + ms.layout->fields[i].offset);
    return nullptr;
}

// Generation 1 as a C++ script. The native virtual Tick runs the script code.
TEST_P(BackendTests, CppScriptRunsThroughNativeVirtual)
{
    spp::LoadResult v1 = backend->load(source("cpp_player_v1.cpp"), 1);
    ASSERT_TRUE(v1) << v1.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Player"));
    s.commit_at_frame_boundary();
    EXPECT_EQ(spp::active_generation, 1u);
    ASSERT_NE(fld<float>(s.inst.shell, "hp"), nullptr);
    EXPECT_EQ(*fld<float>(s.inst.shell, "hp"), 100.0f);
    EXPECT_EQ(*fld<float>(s.inst.shell, "speed"), 5.0f);
    ASSERT_NE(modvar<int>(s, "total"), nullptr);
    EXPECT_EQ(*modvar<int>(s, "total"), 0);

    s.actor->TakeDamage(30.0f);
    EXPECT_EQ(*fld<float>(s.inst.shell, "hp"), 70.0f);
    s.actor->Tick(kDt); // heal 1, total + 1, Move by speed * dt
    EXPECT_EQ(*fld<float>(s.inst.shell, "hp"), 71.0f);
    EXPECT_EQ(*fld<float>(s.inst.shell, "speed"), 5.0f);
    EXPECT_EQ(*modvar<int>(s, "total"), 1);
    EXPECT_FLOAT_EQ(s.actor->Position.x, 5.0f * kDt);
    EXPECT_EQ(*fld<int>(s.inst.shell, "level"), 1);
}

// Default arguments of a .cpp script: a constructor, a constant and a module variable.
TEST_P(BackendTests, CppScriptDefaultArguments)
{
    spp::LoadResult v1 = backend->load(source("cpp_defaults.cpp"), 1);
    ASSERT_TRUE(v1) << v1.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Counter"));
    EXPECT_EQ(*fld<int>(s.inst.shell, "value"), 6); // the default "kStep * 3" of the constructor
    s.actor->Tick(kDt); // Add(10): 10 + 2 + calls(0). Add(10, 5, 100): 115.
    EXPECT_EQ(*fld<int>(s.inst.shell, "value"), 12);
    EXPECT_EQ(*fld<int>(s.inst.shell, "last"), 115);
    EXPECT_EQ(*modvar<int>(s, "calls"), 2);
    s.actor->Tick(kDt); // the default "calls" reads the module variable at each call: 2
    EXPECT_EQ(*fld<int>(s.inst.shell, "value"), 14);
    EXPECT_EQ(*modvar<int>(s, "calls"), 4);
}

// A free function reads and writes the module variable. A method uses the same variable. A reload
// migrates the variable, and the free function then uses the migrated value.
TEST_P(BackendTests, CppScriptFreeFunctionUsesModuleVariable)
{
    spp::LoadResult v1 = backend->load(source("cpp_freefn.cpp"), 1);
    ASSERT_TRUE(v1) << v1.error;
    spp::LoadResult v2 = backend->load(source("cpp_freefn.cpp"), 2);
    ASSERT_TRUE(v2) << v2.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Counter"));
    s.actor->Tick(kDt); // add_total(3): total = 6 + 1 = 7. The method adds 100.
    EXPECT_EQ(*fld<int>(s.inst.shell, "last"), 7);
    ASSERT_NE(modvar<int>(s, "total"), nullptr);
    EXPECT_EQ(*modvar<int>(s, "total"), 107);

    ASSERT_RELOAD_OK(s.loader.request_reload(std::move(*v2.module)));
    const spp::ReloadResult res = s.loader.commit_pending();
    ASSERT_TRUE(res) << res.error;
    EXPECT_EQ(spp::active_generation, 2u);
    ASSERT_NE(modvar<int>(s, "total"), nullptr);
    EXPECT_EQ(*modvar<int>(s, "total"), 107); // migrated

    s.actor->Tick(kDt); // add_total(3): 107 + 7 = 114. The method adds 100.
    EXPECT_EQ(*fld<int>(s.inst.shell, "last"), 114);
    EXPECT_EQ(*modvar<int>(s, "total"), 214);
}

// A function template uses a module variable. The method and the template share the value, also
// after a reload.
TEST_P(BackendTests, CppScriptTemplateUsesModuleVariable)
{
    spp::LoadResult v1 = backend->load(source("cpp_template_fn.cpp"), 1);
    ASSERT_TRUE(v1) << v1.error;
    spp::LoadResult v2 = backend->load(source("cpp_template_fn.cpp"), 2);
    ASSERT_TRUE(v2) << v2.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Scaler"));
    s.actor->Tick(kDt); // total = 2. scaled(3) = 6.
    EXPECT_EQ(*fld<int>(s.inst.shell, "last"), 6);
    ASSERT_NE(modvar<int>(s, "total"), nullptr);
    EXPECT_EQ(*modvar<int>(s, "total"), 2);

    ASSERT_RELOAD_OK(s.loader.request_reload(std::move(*v2.module)));
    const spp::ReloadResult res = s.loader.commit_pending();
    ASSERT_TRUE(res) << res.error;
    EXPECT_EQ(spp::active_generation, 2u);
    ASSERT_NE(modvar<int>(s, "total"), nullptr);
    EXPECT_EQ(*modvar<int>(s, "total"), 2); // migrated

    s.actor->Tick(kDt); // total = 4. scaled(3) = 12.
    EXPECT_EQ(*fld<int>(s.inst.shell, "last"), 12);
    EXPECT_EQ(*modvar<int>(s, "total"), 4);
}

// A function in a namespace uses a module variable. The method and the function share the value,
// also after a reload.
TEST_P(BackendTests, CppScriptNamespaceFunctionUsesModuleVariable)
{
    spp::LoadResult v1 = backend->load(source("cpp_namespace_fn.cpp"), 1);
    ASSERT_TRUE(v1) << v1.error;
    spp::LoadResult v2 = backend->load(source("cpp_namespace_fn.cpp"), 2);
    ASSERT_TRUE(v2) << v2.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Bumper"));
    s.actor->Tick(kDt); // counter::bump(5): total = 5. The method adds 100.
    EXPECT_EQ(*fld<int>(s.inst.shell, "last"), 5);
    ASSERT_NE(modvar<int>(s, "total"), nullptr);
    EXPECT_EQ(*modvar<int>(s, "total"), 105);

    ASSERT_RELOAD_OK(s.loader.request_reload(std::move(*v2.module)));
    const spp::ReloadResult res = s.loader.commit_pending();
    ASSERT_TRUE(res) << res.error;
    EXPECT_EQ(spp::active_generation, 2u);
    ASSERT_NE(modvar<int>(s, "total"), nullptr);
    EXPECT_EQ(*modvar<int>(s, "total"), 105); // migrated

    s.actor->Tick(kDt); // counter::bump(5): total = 110. The method adds 100.
    EXPECT_EQ(*fld<int>(s.inst.shell, "last"), 110);
    EXPECT_EQ(*modvar<int>(s, "total"), 210);
}

// The reload to generation 2. The alias moves hp to health. The convert hooks change level and total.
TEST_P(BackendTests, CppScriptReloadMigratesFieldsAndModuleVariable)
{
    spp::LoadResult v1 = backend->load(source("cpp_player_v1.cpp"), 1);
    ASSERT_TRUE(v1) << v1.error;
    spp::LoadResult v2 = backend->load(source("cpp_player_v2.cpp"), 2);
    ASSERT_TRUE(v2) << v2.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Player"));
    s.actor->TakeDamage(30.0f);
    s.actor->Tick(kDt); // hp 71, total 1
    ASSERT_EQ(*modvar<int>(s, "total"), 1);

    std::string error;
    ASSERT_RELOAD_OK(s.loader.request_reload(std::move(*v2.module)));
    const spp::ReloadResult res = s.loader.commit_pending();
    ASSERT_TRUE(res) << res.error;
    EXPECT_EQ(spp::active_generation, 2u);

    EXPECT_EQ(fld<float>(s.inst.shell, "hp"), nullptr); // the name is "health" now
    ASSERT_NE(fld<float>(s.inst.shell, "health"), nullptr);
    EXPECT_EQ(*fld<float>(s.inst.shell, "health"), 71.0f); // onReload did not change it
    ASSERT_NE(fld<float>(s.inst.shell, "level"), nullptr);
    EXPECT_FLOAT_EQ(*fld<float>(s.inst.shell, "level"), 10.0f); // convert_level(1)
    ASSERT_NE(modvar<float>(s, "total"), nullptr);
    EXPECT_FLOAT_EQ(*modvar<float>(s, "total"), 2.0f); // convert_total(1)
    EXPECT_EQ(*fld<float>(s.inst.shell, "speed"), 5.0f);

    s.actor->Tick(kDt); // the Tick of generation 2
    EXPECT_FLOAT_EQ(*modvar<float>(s, "total"), 3.0f);
    EXPECT_FLOAT_EQ(s.actor->Position.x, 5.0f * kDt + 2.0f * 5.0f * kDt);
}

// Static members of a script class. A static data member is a module variable "Counter__s_count".
// A static method is a free function. A reload keeps the value of a static data member. A new
// static data member takes its initializer. The hook convert_count converts the changed type.
TEST_P(BackendTests, CppScriptStaticMembersMigrate)
{
    spp::LoadResult v1 = backend->load(source("cpp_static_v1.cpp"), 1);
    ASSERT_TRUE(v1) << v1.error;
    spp::LoadResult v2 = backend->load(source("cpp_static_v2.cpp"), 2);
    ASSERT_TRUE(v2) << v2.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Counter"));
    ASSERT_NE(modvar<int>(s, "Counter__s_count"), nullptr);
    EXPECT_EQ(*modvar<int>(s, "Counter__s_count"), 10); // the definition outside the class
    EXPECT_EQ(*modvar<int>(s, "Counter__s_hits"), 5); // the inline initializer
    EXPECT_EQ(*modvar<int>(s, "snapshot"), 11); // a module variable reads a static data member

    s.actor->Tick(kDt);
    s.actor->Tick(kDt); // count 12, hits 9
    EXPECT_EQ(*modvar<int>(s, "Counter__s_count"), 12);
    EXPECT_EQ(*modvar<int>(s, "Counter__s_hits"), 9);
    EXPECT_EQ(*fld<int>(s.inst.shell, "last"), 36); // a method calls the static method scale(12)
    EXPECT_EQ(*fld<int>(s.inst.shell, "via_free"), 45); // a free function: scale(12) + hits

    ASSERT_RELOAD_OK(s.loader.request_reload(std::move(*v2.module)));
    const spp::ReloadResult res = s.loader.commit_pending();
    ASSERT_TRUE(res) << res.error;
    EXPECT_EQ(spp::active_generation, 2u);
    ASSERT_NE(modvar<float>(s, "Counter__s_count"), nullptr);
    EXPECT_FLOAT_EQ(*modvar<float>(s, "Counter__s_count"), 12.5f); // convert_count(12)
    EXPECT_EQ(*modvar<int>(s, "Counter__s_hits"), 9); // migrated
    ASSERT_NE(modvar<int>(s, "Counter__s_extra"), nullptr);
    EXPECT_EQ(*modvar<int>(s, "Counter__s_extra"), 7); // the new static takes its initializer
    EXPECT_EQ(*modvar<int>(s, "snapshot"), 11);

    s.actor->Tick(kDt); // count 13.5, hits 11, extra 8
    EXPECT_FLOAT_EQ(*modvar<float>(s, "Counter__s_count"), 13.5f);
    EXPECT_EQ(*modvar<int>(s, "Counter__s_hits"), 11);
    EXPECT_EQ(*modvar<int>(s, "Counter__s_extra"), 8);
    EXPECT_EQ(*fld<int>(s.inst.shell, "last"), 47); // plus(scale(13), 8): a new static method
    EXPECT_EQ(*fld<int>(s.inst.shell, "via_free"), 50); // scale(13) + hits
}

// Generation 2 moves the method bodies out of the class and changes the body of hit. The fields
// and the module variables keep their values. The new code runs after the reload.
TEST_P(BackendTests, CppScriptOutOfClassDefinitionsReload)
{
    spp::LoadResult v1 = backend->load(source("cpp_outofclass_v1.cpp"), 1);
    ASSERT_TRUE(v1) << v1.error;
    spp::LoadResult v2 = backend->load(source("cpp_outofclass_v2.cpp"), 2);
    ASSERT_TRUE(v2) << v2.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Fighter"));
    s.actor->Tick(kDt);
    s.actor->Tick(kDt);
    EXPECT_EQ(*modvar<int>(s, "ticks"), 2);
    EXPECT_EQ(*fld<int>(s.inst.shell, "hp"), 18); // hit(1) twice
    EXPECT_EQ(*fld<int>(s.inst.shell, "last"), 18);
    EXPECT_FLOAT_EQ(s.actor->Position.x, 2.0f * kDt);

    ASSERT_RELOAD_OK(s.loader.request_reload(std::move(*v2.module)));
    const spp::ReloadResult res = s.loader.commit_pending();
    ASSERT_TRUE(res) << res.error;
    EXPECT_EQ(spp::active_generation, 2u);
    EXPECT_EQ(*modvar<int>(s, "ticks"), 2); // kept: the new constructor did not run
    EXPECT_EQ(*fld<int>(s.inst.shell, "hp"), 18); // kept

    s.actor->Tick(kDt); // the definition outside the class: hit(1) takes 2, Move is twice as fast
    EXPECT_EQ(*modvar<int>(s, "ticks"), 3);
    EXPECT_EQ(*fld<int>(s.inst.shell, "hp"), 16);
    EXPECT_EQ(*fld<int>(s.inst.shell, "last"), 16);
    EXPECT_FLOAT_EQ(s.actor->Position.x, 2.0f * kDt + 2.0f * kDt);
}

// A static member can have the name of a function that the generated code has for the class.
TEST_P(BackendTests, CppScriptStaticMembersWithGeneratedNames)
{
    spp::LoadResult v1 = backend->load(source("cpp_static_names.cpp"), 1);
    ASSERT_TRUE(v1) << v1.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Names"));
    s.actor->Tick(kDt);
    EXPECT_EQ(*fld<int>(s.inst.shell, "last"), 4321);
}

// A static data member changes its type without a convert hook. It follows the rule of a module
// variable: a warning, and the new variable takes its default value.
TEST_P(BackendTests, CppScriptStaticMemberRetypeWithoutHook)
{
    spp::LoadResult v1 = backend->load(source("cpp_retype_v1.cpp"), 1);
    ASSERT_TRUE(v1) << v1.error;
    spp::LoadResult v2 = backend->load(source("cpp_retype_v2.cpp"), 2);
    ASSERT_TRUE(v2) << v2.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Gauge"));
    s.actor->Tick(kDt);
    EXPECT_EQ(*modvar<int>(s, "Gauge__s_reading"), 11);
    EXPECT_EQ(*modvar<int>(s, "level"), 11);

    ASSERT_RELOAD_OK(s.loader.request_reload(std::move(*v2.module)));
    testing::internal::CaptureStderr();
    const spp::ReloadResult res = s.loader.commit_pending();
    const std::string log = testing::internal::GetCapturedStderr();
    ASSERT_TRUE(res) << res.error;
    ASSERT_NE(modvar<float>(s, "Gauge__s_reading"), nullptr);
    ASSERT_NE(modvar<float>(s, "level"), nullptr);
    // The static data member and the module variable get the same value.
    EXPECT_FLOAT_EQ(*modvar<float>(s, "Gauge__s_reading"), *modvar<float>(s, "level"));
    EXPECT_NE(log.find("field Gauge__s_reading changed its type"), std::string::npos) << log;
    EXPECT_NE(log.find("field level changed its type"), std::string::npos) << log;
}

// The hook onReload runs at the reload and reads the old value through its old name.
TEST_P(BackendTests, CppScriptOnReloadRuns)
{
    spp::LoadResult v1 = backend->load(source("cpp_player_v1.cpp"), 1);
    ASSERT_TRUE(v1) << v1.error;
    spp::LoadResult v2 = backend->load(source("cpp_player_v2.cpp"), 2);
    ASSERT_TRUE(v2) << v2.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Player"));
    *fld<float>(s.inst.shell, "hp") = 150.0f; // more than the maximum

    std::string error;
    ASSERT_RELOAD_OK(s.loader.request_reload(std::move(*v2.module)));
    const spp::ReloadResult res = s.loader.commit_pending();
    ASSERT_TRUE(res) << res.error;
    ASSERT_NE(fld<float>(s.inst.shell, "health"), nullptr);
    EXPECT_EQ(*fld<float>(s.inst.shell, "health"), 100.0f); // onReload set it
}

// Script code creates a script object with "new" and keeps the pointer in a module variable.
// The object, its state and the casts on it work in generation 1 and after the reload.
TEST_P(BackendTests, CppScriptObjectsSurviveReload)
{
    spp::LoadResult v1 = backend->load(source("cpp_objects_v1.cpp"), 1);
    ASSERT_TRUE(v1) << v1.error;
    spp::LoadResult v2 = backend->load(source("cpp_objects_v2.cpp"), 2);
    ASSERT_TRUE(v2) << v2.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Spawner"));
    // The variable holds a pointer to the shell. "Actor" is the first base of the shell.
    Actor** slot = modvar<Actor*>(s, "minion");
    ASSERT_NE(slot, nullptr);
    EXPECT_EQ(*slot, nullptr);

    s.actor->Tick(kDt); // new Minion(3), boost(2): power 5, hits 1
    s.actor->Tick(kDt); // boost(2): power 7, hits 2
    ASSERT_NE(*slot, nullptr);
    Actor* minion = *slot;
    auto* shell = dynamic_cast<spp::ShellBase*>(minion);
    ASSERT_NE(shell, nullptr);
    EXPECT_EQ(*fld<int>(shell, "power"), 7);
    EXPECT_EQ(*fld<int>(shell, "hits"), 2);
    EXPECT_EQ(*modvar<int>(s, "cast_hits"), 2);
    EXPECT_EQ(*fld<int>(s.inst.shell, "last"), 7 + 9 + 1); // power + total() + 1
    // The static method "spawn" made a second object. The static data member holds it.
    Actor** made = modvar<Actor*>(s, "Minion__s_first_made");
    ASSERT_NE(made, nullptr);
    ASSERT_NE(*made, nullptr);
    Actor* first = *made;
    EXPECT_EQ(*fld<int>(dynamic_cast<spp::ShellBase*>(first), "power"), 1);
    EXPECT_FLOAT_EQ(minion->Position.x, 2.0f * kDt);

    ASSERT_RELOAD_OK(s.loader.request_reload(std::move(*v2.module)));
    const spp::ReloadResult res = s.loader.commit_pending();
    ASSERT_TRUE(res) << res.error;
    EXPECT_EQ(spp::active_generation, 2u);
    ASSERT_EQ(*modvar<Actor*>(s, "minion"), minion); // the same object
    EXPECT_EQ(*fld<int>(shell, "power"), 7);
    EXPECT_EQ(*fld<int>(shell, "hits"), 2);
    ASSERT_NE(fld<int>(shell, "shield"), nullptr);
    EXPECT_EQ(*fld<int>(shell, "shield"), 5);

    s.actor->Tick(kDt); // generation 2: boost(2) adds 4
    EXPECT_EQ(*fld<int>(shell, "power"), 11);
    EXPECT_EQ(*fld<int>(shell, "hits"), 3);
    EXPECT_EQ(*modvar<int>(s, "cast_hits"), 3); // the casts of generation 2 find the object
    EXPECT_EQ(*fld<int>(s.inst.shell, "last"), 11 + 14 + 1);
    EXPECT_EQ(*modvar<Actor*>(s, "Minion__s_first_made"), first);
    EXPECT_EQ(*fld<int>(dynamic_cast<spp::ShellBase*>(first), "shield"), 5);

    *modvar<Actor*>(s, "minion") = nullptr;
    delete minion; // the virtual destructor of the shell runs
    *modvar<Actor*>(s, "Minion__s_first_made") = nullptr;
    delete first;
}

// A constructor member initializer list. The fields get their values in the order of the
// declarations, and an entry can use the parameters and the earlier fields. After a reload the
// old object keeps its state, and a new object runs the new initializers.
TEST_P(BackendTests, CppScriptMemberInitializersReload)
{
    spp::LoadResult v1 = backend->load(source("cpp_ctor_init_v1.cpp"), 1);
    ASSERT_TRUE(v1) << v1.error;
    spp::LoadResult v2 = backend->load(source("cpp_ctor_init_v2.cpp"), 2);
    ASSERT_TRUE(v2) << v2.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Spawner"));
    s.actor->Tick(kDt); // new Unit(4, "bob")
    Actor** slot = modvar<Actor*>(s, "first");
    ASSERT_NE(slot, nullptr);
    ASSERT_NE(*slot, nullptr);
    Actor* first = *slot;
    auto* shell = dynamic_cast<spp::ShellBase*>(first);
    ASSERT_NE(shell, nullptr);
    EXPECT_EQ(*fld<int>(shell, "hp"), 8); // hp_ * 2
    EXPECT_EQ(*fld<int>(shell, "tag"), 1); // the default argument, with the brace form
    EXPECT_EQ(*fld<std::string>(shell, "name"), "bob");
    EXPECT_EQ(*fld<int>(shell, "twice"), 9); // hp + tag: it sees the new hp and tag
    EXPECT_EQ(*fld<int>(shell, "len"), 4); // the length of Name: "Unit"
    EXPECT_EQ(*fld<int>(shell, "stamp"), 6); // the default 5, then the body
    EXPECT_EQ(*fld<int>(shell, "kept"), 7); // a default member initializer with no entry

    *fld<int>(shell, "kept") = 70;
    ASSERT_RELOAD_OK(s.loader.request_reload(std::move(*v2.module)));
    const spp::ReloadResult res = s.loader.commit_pending();
    ASSERT_TRUE(res) << res.error;
    EXPECT_EQ(spp::active_generation, 2u);
    EXPECT_EQ(*fld<int>(shell, "hp"), 8); // kept: the constructor did not run again
    EXPECT_EQ(*fld<std::string>(shell, "name"), "bob");
    EXPECT_EQ(*fld<int>(shell, "kept"), 70);
    EXPECT_EQ(*fld<int>(shell, "extra"), 0); // the new field has the default value

    s.actor->Tick(kDt); // new Unit(4, "bob") with the new initializers
    Actor** slot2 = modvar<Actor*>(s, "second");
    ASSERT_NE(slot2, nullptr);
    ASSERT_NE(*slot2, nullptr);
    Actor* second = *slot2;
    auto* shell2 = dynamic_cast<spp::ShellBase*>(second);
    ASSERT_NE(shell2, nullptr);
    EXPECT_EQ(*fld<int>(shell2, "hp"), 12); // hp_ * 3
    EXPECT_EQ(*fld<int>(shell2, "tag"), 10);
    EXPECT_EQ(*fld<std::string>(shell2, "name"), "bob!");
    EXPECT_EQ(*fld<int>(shell2, "twice"), 22);
    EXPECT_EQ(*fld<int>(shell2, "extra"), 4);
    EXPECT_EQ(*fld<int>(shell2, "kept"), 7);

    *modvar<Actor*>(s, "first") = nullptr;
    *modvar<Actor*>(s, "second") = nullptr;
    delete first;
    delete second;
}

// A script class derives from a script class. Kid inherits the fields and the methods of Base,
// overrides the virtual Power of Base, and calls the versions of Base with "Base::". The object
// of generation 1 survives the reload. Generation 2 adds an override of TakeDamage and the new
// virtual Bonus. The cast from Base* to Kid* finds the object in both generations.
TEST_P(BackendTests, CppScriptChildClassReload)
{
    spp::LoadResult v1 = backend->load(source("cpp_inherit_v1.cpp"), 1);
    ASSERT_TRUE(v1) << v1.error;
    spp::LoadResult v2 = backend->load(source("cpp_inherit_v2.cpp"), 2);
    ASSERT_TRUE(v2) << v2.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Kid"));
    spp::ShellBase* shell = s.inst.shell;
    ASSERT_NE(shell->cls->super, nullptr);
    EXPECT_STREQ(shell->cls->super->name, "Base");
    EXPECT_EQ(*fld<int>(shell, "hits"), 6); // Kid(3) gives Base(6)
    EXPECT_EQ(*fld<int>(shell, "kid_hits"), 3);

    s.actor->Tick(kDt);
    EXPECT_FLOAT_EQ(*fld<float>(shell, "moved"), 1.0f); // the native Move through the child
    EXPECT_EQ(*fld<int>(shell, "hits"), 16); // the inherited Bump
    EXPECT_EQ(*fld<int>(shell, "twice"), 10); // the inherited Twice calls the virtual Kid::Power (5)
    EXPECT_EQ(*fld<int>(shell, "super_power"), 1); // Base::Power() is the version of Base
    EXPECT_EQ(*fld<int>(shell, "seen_hits"), 19); // the fields of the object through the cast
    EXPECT_EQ(*modvar<int>(s, "kid_casts"), 1);
    ASSERT_NE(modvar<Actor*>(s, "last_ticked"), nullptr);
    EXPECT_EQ(*modvar<Actor*>(s, "last_ticked"), s.actor); // "this" is the object

    s.actor->TakeDamage(3.0f); // Kid has no override yet: the override of Base runs
    EXPECT_FLOAT_EQ(*fld<float>(shell, "hp"), 97.0f);
    EXPECT_EQ(*fld<int>(shell, "hits"), 17);
    s.actor->Tick(kDt);
    EXPECT_EQ(*fld<int>(shell, "hits"), 27);
    EXPECT_EQ(*modvar<int>(s, "kid_casts"), 2);

    ASSERT_RELOAD_OK(s.loader.request_reload(std::move(*v2.module)));
    const spp::ReloadResult res = s.loader.commit_pending();
    ASSERT_TRUE(res) << res.error;
    EXPECT_EQ(spp::active_generation, 2u);
    EXPECT_EQ(s.inst.shell, shell); // the same object
    EXPECT_EQ(shell->cls->super->generation, 2u);
    EXPECT_EQ(*fld<int>(shell, "hits"), 27); // the parent state stays
    EXPECT_FLOAT_EQ(*fld<float>(shell, "hp"), 97.0f);
    EXPECT_EQ(*fld<int>(shell, "kid_hits"), 3); // the child state stays
    EXPECT_EQ(*fld<int>(shell, "armor"), 4); // the new parent field has its initializer
    EXPECT_EQ(*fld<int>(shell, "gen"), 2); // the new child field

    s.actor->Tick(kDt);
    EXPECT_FLOAT_EQ(*fld<float>(shell, "moved"), 3.0f);
    EXPECT_EQ(*fld<int>(shell, "hits"), 37);
    EXPECT_EQ(*fld<int>(shell, "twice"), 14); // Twice of Base now calls Kid::Power (7)
    EXPECT_EQ(*fld<int>(shell, "super_power"), 1);
    EXPECT_EQ(*fld<int>(shell, "bonus"), 20); // the new virtual
    EXPECT_EQ(*fld<int>(shell, "seen_hits"), 37 + 3 + 20);
    EXPECT_EQ(*modvar<int>(s, "kid_casts"), 3); // the cast of generation 2 finds the old object

    s.actor->TakeDamage(2.0f); // the new override: Base::TakeDamage(6)
    EXPECT_FLOAT_EQ(*fld<float>(shell, "hp"), 91.0f);
    EXPECT_EQ(*fld<int>(shell, "hits"), 38);
    EXPECT_EQ(*fld<int>(shell, "kid_hits"), 4);
}

// A chain of three script classes: Leaf derives from Mid, and Mid derives from Base. The object
// of generation 1 survives the reload with the state of the three classes. Generation 2 adds the
// virtual method Extra to Base. Mid and Leaf override it, and Base calls it.
TEST_P(BackendTests, CppScriptChainReload)
{
    spp::LoadResult v1 = backend->load(source("cpp_chain_v1.cpp"), 1);
    ASSERT_TRUE(v1) << v1.error;
    spp::LoadResult v2 = backend->load(source("cpp_chain_v2.cpp"), 2);
    ASSERT_TRUE(v2) << v2.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Leaf"));
    spp::ShellBase* shell = s.inst.shell;
    ASSERT_NE(shell->cls->super, nullptr);
    ASSERT_NE(shell->cls->super->super, nullptr);
    EXPECT_STREQ(shell->cls->super->name, "Mid");
    EXPECT_STREQ(shell->cls->super->super->name, "Base");

    s.actor->Tick(kDt); // Leaf::Step is Mid::Step (10) + 100
    s.actor->Tick(kDt);
    EXPECT_EQ(*fld<int>(shell, "total"), 220);
    EXPECT_EQ(*fld<int>(shell, "mid"), 5);
    EXPECT_EQ(*fld<int>(shell, "leaf"), 8);
    EXPECT_EQ(*modvar<int>(s, "mids"), 2); // the cast to Mid finds the Leaf object

    ASSERT_RELOAD_OK(s.loader.request_reload(std::move(*v2.module)));
    const spp::ReloadResult res = s.loader.commit_pending();
    ASSERT_TRUE(res) << res.error;
    EXPECT_EQ(s.inst.shell, shell);
    EXPECT_EQ(shell->cls->super->super->generation, 2u);
    EXPECT_EQ(*fld<int>(shell, "total"), 220); // the state of each class stays
    EXPECT_EQ(*fld<int>(shell, "mid"), 5);
    EXPECT_EQ(*fld<int>(shell, "leaf"), 8);
    EXPECT_EQ(*fld<int>(shell, "armor"), 4); // the new fields have their initializers
    EXPECT_EQ(*fld<int>(shell, "leaf2"), 7);

    s.actor->Tick(kDt); // Step: 10 + 200. Extra: the override of Mid (1000) + 1.
    EXPECT_EQ(*fld<int>(shell, "total"), 220 + 210 + 1001);
    EXPECT_EQ(*fld<int>(shell, "leaf"), 9);
    EXPECT_EQ(*modvar<int>(s, "mids"), 3);
}

// Lambdas in the methods and in the constructor. A lambda reaches the fields through the shell,
// not through a pointer to the state. A reload moves the state to a new block. The lambda in the
// field "stored" is from generation 1. It still works after the reload and reads the new state.
// The old generation stays loaded, because a live field can hold code.
TEST_P(BackendTests, CppScriptLambdasUseTheShell)
{
    spp::LoadResult v1 = backend->load(source("cpp_lambda_v1.cpp"), 1);
    ASSERT_TRUE(v1) << v1.error;
    spp::LoadResult v2 = backend->load(source("cpp_lambda_v2.cpp"), 2);
    ASSERT_TRUE(v2) << v2.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Lamp"));
    EXPECT_EQ(*fld<int>(s.inst.shell, "level"), 6); // the lambda of the constructor: 3 * 2

    s.actor->Tick(kDt);
    // [this]: bump(1) gives 1. [&]: +10 and bump(2) give 13.
    EXPECT_EQ(*fld<int>(s.inst.shell, "count"), 13);
    EXPECT_EQ(*fld<int>(s.inst.shell, "sum"), 7); // [=]: 1 + 6
    EXPECT_EQ(*fld<int>(s.inst.shell, "gen"), 113); // the generic lambda
    EXPECT_EQ(*fld<int>(s.inst.shell, "nested"), 13 * 2 + 6); // the nested lambda
    EXPECT_TRUE(*fld<bool>(s.inst.shell, "flag")); // "this" as a value
    EXPECT_EQ(*modvar<int>(s, "calls"), 1);
    EXPECT_EQ(*fld<int>(s.inst.shell, "stored_result"), 1013);
    EXPECT_FLOAT_EQ(s.actor->Position.x, kDt);
    EXPECT_FLOAT_EQ(s.actor->Position.y, kDt);
    // The method returned. The stored lambdas use the object, not a local of the method.
    using Fn = std::function<int()>;
    ASSERT_NE(fld<Fn>(s.inst.shell, "kept_ref"), nullptr);
    EXPECT_EQ((*fld<Fn>(s.inst.shell, "kept_ref"))(), 13 * 3); // [&]
    EXPECT_EQ((*fld<Fn>(s.inst.shell, "kept_init"))(), 13 + 5); // [self = this]
    EXPECT_EQ((*fld<Fn>(s.inst.shell, "from_init"))(), 13 + 6); // a lambda in a field initializer

    ASSERT_RELOAD_OK(s.loader.request_reload(std::move(*v2.module)));
    const spp::ReloadResult res = s.loader.commit_pending();
    ASSERT_TRUE(res) << res.error;
    EXPECT_EQ(spp::active_generation, 2u);
    EXPECT_EQ(*fld<int>(s.inst.shell, "count"), 13); // kept

    s.actor->Tick(kDt); // bump adds 5 more in generation 2: 13 + 6 + 10 + 7 = 36
    EXPECT_EQ(*fld<int>(s.inst.shell, "count"), 36);
    EXPECT_EQ(*fld<int>(s.inst.shell, "sum"), 19 + 6);
    EXPECT_EQ(*fld<int>(s.inst.shell, "gen"), 136);
    EXPECT_EQ(*fld<int>(s.inst.shell, "nested"), 36 * 2 + 6);
    EXPECT_EQ(*modvar<int>(s, "calls"), 2);
    // The stored lambda is the one of generation 1 (+1000), not the one of generation 2 (+2000).
    EXPECT_EQ(*fld<int>(s.inst.shell, "stored_result"), 1036);

    EXPECT_EQ((*fld<Fn>(s.inst.shell, "kept_ref"))(), 36 * 3);
    EXPECT_EQ((*fld<Fn>(s.inst.shell, "kept_init"))(), 36 + 5);
    EXPECT_EQ((*fld<Fn>(s.inst.shell, "from_init"))(), 36 + 6);

    // The stored lambda holds code of generation 1: the generation does not unload.
    s.loader.frame_boundary();
    s.loader.frame_boundary();
    ASSERT_EQ(s.loader.stats().generations.size(), 2u);
    EXPECT_NE(s.loader.stats().generations[0].state, spp::GenState::Unloaded);
    EXPECT_TRUE(s.loader.stats().state_holds_code);
}

// A .spp script and a .cpp script live in the same process. Each has its own loader and one object.
// One loader holds one module for each generation, so the two scripts use two loaders.
TEST_P(BackendTests, SppAndCppScriptsRunSideBySide)
{
    spp::LoadResult spp_mod = backend->load(source("echo_v1.spp"), 1);
    ASSERT_TRUE(spp_mod) << spp_mod.error;
    spp::LoadResult cpp_mod = backend->load(source("cpp_player_v1.cpp"), 1);
    ASSERT_TRUE(cpp_mod) << cpp_mod.error;
    Session echo, player;
    ASSERT_NO_FATAL_FAILURE(echo.start(std::move(*spp_mod.module), "Echo"));
    ASSERT_NO_FATAL_FAILURE(player.start(std::move(*cpp_mod.module), "Player"));

    echo.world.Tick(kDt);
    echo.world.Tick(kDt);
    player.world.Tick(kDt);
    player.world.Tick(kDt);
    EXPECT_EQ(*fld<int>(echo.inst.shell, "ticks"), 2);
    EXPECT_EQ(*fld<float>(player.inst.shell, "hp"), 100.0f); // heal is clamped
    EXPECT_EQ(*modvar<int>(player, "total"), 2);
    EXPECT_FLOAT_EQ(player.actor->Position.x, 2.0f * 5.0f * kDt);
}

} // namespace
