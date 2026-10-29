// Tests of script properties with the backend that SPP_BACKEND selected.
//
// CMake defines the macro SPP_SOURCE_DIR.
#include <gtest/gtest.h>

#include <memory>
#include <stdexcept>
#include <string>

#include "spp/backend.h"
#include "support/backend_fixture.h"
#include "support/script_files.h"
#include "support/test_support.h"

namespace {

using test_support::kDt;
using test_support::Session;
using test_support::source;

// A script with properties: an auto property, a read-only property, a property with a private
// set accessor and a custom getter and setter. The hidden fields are normal state fields.
TEST_P(BackendTests, PropertiesReadAndWrite)
{
    spp::LoadResult v1 = backend->load(source("props_v1.spp"), 1);
    ASSERT_TRUE(v1) << v1.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Gauge"));
    ASSERT_NE(s.field("__p_Hp"), nullptr);
    ASSERT_NE(s.field("__p_MaxHp"), nullptr);
    ASSERT_NE(s.field("shield_raw"), nullptr);
    EXPECT_EQ(s.field("__p_Shield"), nullptr);  // a custom property has no hidden field
    EXPECT_EQ(*s.field("__p_Hp"), 50.0f);
    EXPECT_EQ(*s.field("__p_MaxHp"), 100.0f);

    s.world.Tick(kDt);  // Hp += 1, Shield = Shield + 2, Level++
    EXPECT_EQ(*s.field("__p_Hp"), 51.0f);
    EXPECT_EQ(*s.field("shield_raw"), 2.0f);
    const int* level = spp::field_ptr<int>(s.inst.shell, "__p_Level");
    ASSERT_NE(level, nullptr);
    EXPECT_EQ(*level, 2);

    // The custom setter clamps the shield at 0. Then the damage reaches Hp.
    s.actor->TakeDamage(5.0f);
    EXPECT_EQ(*s.field("shield_raw"), 0.0f);
    EXPECT_EQ(*s.field("__p_Hp"), 46.0f);
}

// One script object reads and writes the properties of another script object of the class.
// The accessors run for the other object. An object of another class gives an exception.
TEST_P(BackendTests, PropertiesOfOtherObjectReadAndWrite)
{
    const spp::ScriptSource src = {"props_other.spp", R"(
using "Actor.h";

class Gauge : Actor {
    float Hp { get; set; } = 50.0;
    float MaxHp { get; } = 100.0;
    int Hits { get; private set; } = 0;
    float raw = 0.0;
    float Shield {
        get { return this.raw; }
        set { this.raw = value < 0.0 ? 0.0 : value; }
    }
    float seen = 0.0;
    float chained = 0.0;
    float old_hits = 0.0;

    override void Tick(float dt) {}
    override void TakeDamage(float amount) {}
    override void TakeDamage(Actor& source, float amount) {
        this.seen = source.Hp + source.MaxHp;
        source.Hp -= amount;
        source.Hits++;
        source.Shield = -5.0;
        Actor& p = source;
        p.Shield += 3.0;
        this.chained = source.Hp = this.Hp = 30.0;
        this.old_hits = source.Hits++;
    }
}
)"};
    spp::LoadResult r = backend->load(src, 1);
    ASSERT_TRUE(r) << r.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*r.module), "Gauge"));
    const spp::ClassEntry* entry = s.loader.find("Gauge");
    ASSERT_NE(entry, nullptr);
    const spp::Instance other = entry->create();
    Actor* source = static_cast<Actor*>(other.native);
    s.world.Add(source);

    s.actor->TakeDamage(*source, 5.0f);
    auto f = [](spp::ShellBase* sh, const char* name) { return *spp::field_ptr<float>(sh, name); };
    EXPECT_EQ(f(s.inst.shell, "seen"), 150.0f);  // 50 + 100, read on the other object
    EXPECT_EQ(f(s.inst.shell, "__p_Hp"), 30.0f);
    EXPECT_EQ(f(s.inst.shell, "chained"), 30.0f);
    EXPECT_EQ(f(s.inst.shell, "old_hits"), 1.0f);  // "Hits++" gives the old value
    EXPECT_EQ(f(other.shell, "__p_Hp"), 30.0f);     // 50 - 5, then 30
    EXPECT_EQ(f(other.shell, "raw"), 3.0f);         // the setter clamps -5 to 0, then 0 + 3
    EXPECT_EQ(*spp::field_ptr<int>(other.shell, "__p_Hits"), 2);
    // The accessors did not touch the state of the caller.
    EXPECT_EQ(*spp::field_ptr<int>(s.inst.shell, "__p_Hits"), 0);
    EXPECT_EQ(f(s.inst.shell, "raw"), 0.0f);

    // A native actor is not a Gauge. The property access throws and changes nothing.
    Actor native;
    EXPECT_THROW(s.actor->TakeDamage(native, 1.0f), std::runtime_error);
    EXPECT_EQ(f(other.shell, "__p_Hp"), 30.0f);
}

}  // namespace
