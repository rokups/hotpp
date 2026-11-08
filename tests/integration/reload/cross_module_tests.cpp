// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Reload tests of a script family over two or three units (modules step 5). The child unit imports
// the parent unit with "using". Each unit is its own module. The child state derives from the
// parent state of the interface. The loader fills the inherited slots of the child from the parent.
#include "hot/backend.h"
#include "support/backend_fixture.h"
#include "support/reload_assert.h"
#include "support/script_files.h"
#include "support/test_support.h"

#include <gtest/gtest.h>

#include <filesystem>

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace
{

using test_support::kDt;
using test_support::Session;

template <class T> T* fld(Session& s, const char* name)
{
    return hot::field_ptr<T>(s.inst.shell, name);
}

void reload(Session& s, hot::LoadResult& r, uint32_t gen)
{
    ASSERT_TRUE(r) << r.error;
    std::string error;
    ASSERT_RELOAD_OK(s.loader.request_reload(std::move(*r.module)));
    s.world.Tick(kDt); // the commit happens at the end of this frame
    ASSERT_FALSE(s.commit_failed);
    EXPECT_EQ(hot::active_generation, gen);
}

// The root of a chain over three units: "c.hot".
const char* const kC1 = R"(
using "Actor.h";
class Root : Actor {
    int r = 1;
    virtual int Val() { return 1; }
    override void Tick(float dt) { this.r = this.r + 1; }
}
)";

// A body change of the root: the interface stays the same.
const char* const kC2 = R"(
using "Actor.h";
class Root : Actor {
    int r = 1;
    virtual int Val() { return 5; }
    override void Tick(float dt) { this.r = this.r + 2; }
}
)";

// The middle of the chain: "b.hot". It reads a field of the root and calls the root with "base".
const char* const kB = R"(
using "Actor.h";
using "c.hot";
class Mid : Root {
    int m = 10;
    override int Val() { return base.Val() + this.m + this.r * 0; }
}
)";

// The leaf of the chain: "a.hot". It imports only "b.hot".
const char* const kA = R"(
using "Actor.h";
using "b.hot";
class Leaf : Mid {
    int total = 0;
    override int Val() { return base.Val() + 100; }
    override void Tick(float dt) {
        base.Tick(dt);
        this.total = Val() + this.r * 1000 + this.m;
    }
}
)";

class FamilyTests : public ::testing::TestWithParam<hot::BackendKind>
{
protected:
    void SetUp() override
    {
        if (!hot::backend_available(GetParam()))
            GTEST_SKIP() << "the backend is not in this build";
        hot::DefaultBackendOptions o;
        o.kind = GetParam();
        backend = hot::make_default_backend(o);
    }

    // Builds one unit. "deps" are the texts of the imported units, under their import names.
    hot::LoadResult build(const char* unit, const std::string& text,
        std::vector<std::pair<std::string, std::string>> deps, uint32_t gen)
    {
        // The unit name is the script name without ".hot" (doc/module-level.md, "Unit names").
        hot::ScriptSource src{unit, text, {}, std::filesystem::path(unit).replace_extension().generic_string()};
        for (auto& d : deps)
            src.modules.emplace_back(d.first, d.second);
        hot::LoadResult r = backend->load(src, gen);
        return r;
    }

    // Loads the units as one batch and makes an object of the class "cls".
    void start(Session& s, std::vector<hot::LoadResult> units, const char* cls)
    {
        std::vector<hot::Module> modules;
        for (hot::LoadResult& r : units)
        {
            ASSERT_TRUE(r) << r.error;
            modules.push_back(std::move(*r.module));
        }
        ASSERT_RELOAD_OK(s.loader.load(std::move(modules)));
        const hot::ClassEntry* e = s.loader.find(cls);
        ASSERT_NE(e, nullptr);
        s.inst = e->create();
        s.actor = static_cast<Actor*>(s.inst.native);
        s.world.Add(s.actor);
        s.commit_at_frame_boundary();
    }

    // Requests a batch reload and commits it at the next frame.
    void reload(Session& s, std::vector<hot::LoadResult> units)
    {
        for (const hot::LoadResult& r : units)
            ASSERT_TRUE(r) << r.error;
        ASSERT_RELOAD_OK(s.loader.request_reload(std::move(units)));
        s.world.Tick(kDt);
        ASSERT_FALSE(s.commit_failed);
        ASSERT_FALSE(s.loader.reload_pending());
    }

    std::unique_ptr<hot::Backend> backend;
};

std::string text_of(const char* file)
{
    return test_support::source(file).text;
}

std::vector<hot::LoadResult> batch_of(hot::LoadResult a, hot::LoadResult b)
{
    std::vector<hot::LoadResult> v;
    v.push_back(std::move(a));
    v.push_back(std::move(b));
    return v;
}

// "Grunt" of the child unit derives from "Unit" of the parent unit. A field change of the parent
// needs the child in the batch. A reload of the child alone keeps the parent. The live child object
// keeps its address and its values. The child override stays in its slot after the parent adds a slot.
TEST_P(FamilyTests, FamilyOverTwoUnitsReloadsEachUnit)
{
    const std::string p1 = text_of("xmod_parent_v1.hot"), p2 = text_of("xmod_parent_v2.hot");
    Session s;
    ASSERT_NO_FATAL_FAILURE(start(s,
        batch_of(build("xmod_child.hot", text_of("xmod_child_v1.hot"), {{"xmod_parent.hot", p1}}, 1),
            build("xmod_parent.hot", p1, {}, 1)),
        "Grunt"));
    ASSERT_NE(s.inst.shell->cls->super, nullptr);
    EXPECT_STREQ(s.inst.shell->cls->super->name, "Unit");
    EXPECT_EQ(s.loader.find("Unit")->record, s.inst.shell->cls->super) << "the parent record of the parent unit";
    EXPECT_EQ(*fld<int>(s, "ctor_log"), 12); // Unit(int) of the parent unit first, then Grunt(int)
    EXPECT_EQ(*fld<int>(s, "hp"), 1);
    *fld<int>(s, "rage") = 7;
    s.world.Tick(kDt);
    EXPECT_EQ(*fld<int>(s, "ticks"), 1);
    EXPECT_EQ(*fld<int>(s, "power"), 7); // the parent Tick calls the child override
    void* const shell = s.inst.shell;

    // A new field of the parent changes its interface: the parent alone is rejected.
    std::vector<hot::LoadResult> alone;
    alone.push_back(build("xmod_parent.hot", p2, {}, 2));
    const hot::ReloadResult stale = s.loader.request_reload(std::move(alone));
    EXPECT_EQ(stale.status, hot::ReloadStatus::Failed);
    EXPECT_EQ(stale.reason, hot::ReloadReason::DependentsStale) << stale.error;

    // The parent and the rebuilt child in one batch: the child object migrates.
    ASSERT_NO_FATAL_FAILURE(reload(s,
        batch_of(build("xmod_parent.hot", p2, {}, 3),
            build("xmod_child.hot", text_of("xmod_child_v1.hot"), {{"xmod_parent.hot", p2}}, 3))));
    s.world.Tick(kDt);
    EXPECT_EQ(s.inst.shell, shell);
    EXPECT_EQ(*fld<int>(s, "power"), 107); // the Tick of parent v2 and the Power of child v1
    EXPECT_EQ(*fld<int>(s, "bonus"), 5); // the new parent virtual
    EXPECT_EQ(*fld<int>(s, "armor"), 9); // the new parent field of the old child object
    EXPECT_EQ(*fld<int>(s, "hp"), 1);
    EXPECT_EQ(*fld<int>(s, "rage"), 7);
    EXPECT_EQ(s.inst.shell->cls->super->generation, 3u);

    // The child alone: a changed override, a new override and a new field.
    std::vector<hot::LoadResult> child;
    child.push_back(build("xmod_child.hot", text_of("xmod_child_v2.hot"), {{"xmod_parent.hot", p2}}, 4));
    ASSERT_NO_FATAL_FAILURE(reload(s, std::move(child)));
    s.world.Tick(kDt);
    EXPECT_EQ(s.inst.shell, shell);
    EXPECT_EQ(*fld<int>(s, "power"), 114); // rage * 2 + 100
    EXPECT_EQ(s.inst.shell->cls->slot_count, 5u);
    EXPECT_EQ(*fld<int>(s, "bonus"), 4); // the child override of Bonus reads the new field
    EXPECT_EQ(*fld<int>(s, "armor"), 9);
    EXPECT_EQ(*fld<int>(s, "ticks"), 5); // one tick in each frame, also in the two commit frames
    EXPECT_EQ(s.inst.shell->cls->super->generation, 3u) << "the parent unit is not in the batch";
}

// The child unit builds a child object with "new" and uses it through a parent handle.
TEST_P(FamilyTests, FamilyOverTwoUnitsNewAndIs)
{
    const std::string p1 = text_of("xmod_parent_v1.hot"), p2 = text_of("xmod_parent_v2.hot");
    Session s;
    ASSERT_NO_FATAL_FAILURE(start(s,
        batch_of(build("xmod_parent.hot", p1, {}, 1),
            build("xmod_child.hot", text_of("xmod_child_v1.hot"), {{"xmod_parent.hot", p1}}, 1)),
        "Spawner"));
    s.world.Tick(kDt);
    EXPECT_EQ(*fld<int>(s, "made_power"), 6);
    EXPECT_TRUE(*fld<bool>(s, "made_unit"));
    ASSERT_NO_FATAL_FAILURE(reload(s,
        batch_of(build("xmod_parent.hot", p2, {}, 2),
            build("xmod_child.hot", text_of("xmod_child_v2.hot"), {{"xmod_parent.hot", p2}}, 2))));
    s.world.Tick(kDt);
    EXPECT_EQ(*fld<int>(s, "made_power"), 12);
}

// A chain over three units: Leaf (a.hot) : Mid (b.hot) : Root (c.hot). "base" calls, overrides and
// parent fields cross the units. A body change of the root reloads the root alone: the live leaf
// keeps its state and runs the new root code.
TEST_P(FamilyTests, ChainOverThreeUnitsAndBodyReloadOfTheRoot)
{
    Session s;
    std::vector<hot::LoadResult> first;
    first.push_back(build("a.hot", kA, {{"b.hot", kB}, {"c.hot", kC1}}, 1));
    first.push_back(build("b.hot", kB, {{"c.hot", kC1}}, 1));
    first.push_back(build("c.hot", kC1, {}, 1));
    ASSERT_NO_FATAL_FAILURE(start(s, std::move(first), "Leaf"));
    s.world.Tick(kDt);
    EXPECT_EQ(*fld<int>(s, "total"), 111 + 2000 + 10);
    s.world.Tick(kDt);
    EXPECT_EQ(*fld<int>(s, "total"), 111 + 3000 + 10);
    const hot::ClassRecord* leaf = s.inst.shell->cls;
    ASSERT_NE(leaf->super, nullptr);
    ASSERT_NE(leaf->super->super, nullptr);
    EXPECT_STREQ(leaf->super->super->name, "Root");

    std::vector<hot::LoadResult> root;
    root.push_back(build("c.hot", kC2, {}, 2));
    ASSERT_NO_FATAL_FAILURE(reload(s, std::move(root)));
    EXPECT_EQ(s.inst.shell->cls, leaf);
    EXPECT_EQ(leaf->generation, 1u) << "a.hot is not in the batch";
    s.world.Tick(kDt); // r: 4 with the old code in the commit frame, then 6 with the new code
    EXPECT_EQ(*fld<int>(s, "total"), 115 + 6000 + 10);
}

// G23: the owner unit "lib.hot" holds the static fields of "Counter" in its module block.
const char* kStaticLib1 = R"(
class Counter {
    static int count_ = 0;
    private static int mark_ = 7;
    static int Count { get { return count_; } set { count_ = value; } }
    static int Bump(int by) { count_ += by; return count_; }
    static int Mark() { mark_ += 1; return mark_; }
}
)";

// Generation 2: a new body of "Bump". The interface stays.
const char* kStaticLib2 = R"(
class Counter {
    static int count_ = 0;
    private static int mark_ = 7;
    static int Count { get { return count_; } set { count_ = value; } }
    static int Bump(int by) { count_ += by * 2; return count_; }
    static int Mark() { mark_ += 1; return mark_; }
}
)";

// Generation 3: the private static field "mark_" changes its type. A reload resets it to the
// initializer. The interface stays, because a private static field is not in it.
const char* kStaticLib3 = R"(
class Counter {
    static int count_ = 0;
    private static float mark_ = 2.5f;
    static int Count { get { return count_; } set { count_ = value; } }
    static int Bump(int by) { count_ += by * 2; return count_; }
    static int Mark() { mark_ += 1.0f; return mark_ < 4.0f ? 1 : 0; }
}
)";

// The importer: a static field, a static property and a static method of "Counter" (also from a
// lambda), and a static method and a static property of its own class.
const char* kStaticApp = R"(
using "Actor.h";
using "lib.hot";

class Hero : Actor {
    static int Ticks { get; set; } = 0;
    int seen = 0;
    int got = 0;
    int mark = 0;
    int twice = 0;
    int ticks = 0;

    static int Twice(int v) => v * 2;

    override void Tick(float dt) {
        Counter.count_ += 1;
        Counter.Count = Counter.Count + 10;
        std.function<int()> f = () => Counter.Bump(100);
        this.got = f();
        this.seen = Counter.count_;
        this.mark = Counter.Mark();
        this.twice = Twice(3) + Hero.Twice(1);
        Ticks++;
        this.ticks = Hero.Ticks;
    }
}
)";

// G23: static members over two units. A reload of the owner keeps the values of the static fields
// and runs the new body of a static method. A new type of a static field resets it to the initializer.
// "this" as a handle in a child of a class of another unit. The unit of the parent decides the
// shell of the parent alone: the copy in the importer has the same layout. The child adds
// std::enable_shared_from_this itself, and its own child in the unit uses that base. SSolo uses
// "this" as a handle in its own unit: the copy in the importer has the base of the owner too.
const char* const kShareRoot1 = R"(
using "Actor.h";
class SRoot : Actor {
    int r = 1;
    virtual int Val() { return 1; }
}
class SSolo : Actor {
    int r = 3;
    int Probe() { $SSolo me = this; if (me != null) { return me.r; } return 0; }
}
)";

const char* const kShareRoot2 = R"(
using "Actor.h";
class SRoot : Actor {
    int r = 1;
    virtual int Val() { return 5; }
}
class SSolo : Actor {
    int r = 3;
    int Probe() { $SSolo me = this; if (me != null) { return me.r; } return 0; }
}
)";

const char* const kShareKids1 = R"(
using "Actor.h";
using "sroot.hot";
class SMid : SRoot {
    int m = 10;
    int seen = 0;
    void Poke() { $SMid me = this; if (me != null) { this.seen = me.Val() + me.m; } }
}
class SLeaf : SMid {
    override int Val() { return base.Val() + 100; }
}
class SHost : Actor {
    int got = 0;
    override void Tick(float dt) {
        $SLeaf k = new $SLeaf();
        k.Poke();
        $SMid mid = new $SMid();
        mid.Poke();
        $SSolo solo = new $SSolo();
        this.got = k.seen * 1000 + mid.seen + solo.Probe() * 1000000;
    }
}
)";

const char* const kShareKids2 = R"(
using "Actor.h";
using "sroot.hot";
class SMid : SRoot {
    int m = 10;
    int seen = 0;
    void Poke() { $SMid me = this; if (me != null) { this.seen = me.Val() + me.m; } }
}
class SLeaf : SMid {
    override int Val() { return base.Val() + 200; }
}
class SHost : Actor {
    int got = 0;
    override void Tick(float dt) {
        $SLeaf k = new $SLeaf();
        k.Poke();
        $SMid mid = new $SMid();
        mid.Poke();
        $SSolo solo = new $SSolo();
        this.got = k.seen * 1000 + mid.seen + solo.Probe() * 1000000;
    }
}
)";

std::vector<hot::LoadResult> one_of(hot::LoadResult a)
{
    std::vector<hot::LoadResult> v;
    v.push_back(std::move(a));
    return v;
}

TEST_P(FamilyTests, ThisAsAHandleInAChildOfAnotherUnit)
{
    Session s;
    ASSERT_NO_FATAL_FAILURE(start(s,
        batch_of(build("sroot.hot", kShareRoot1, {}, 1),
            build("skids.hot", kShareKids1, {{"sroot.hot", kShareRoot1}}, 1)),
        "SHost"));
    s.world.Tick(kDt);
    EXPECT_EQ(*fld<int>(s, "got"), 3000000 + 111 * 1000 + 11);
    // A body change of the parent unit keeps the importer.
    ASSERT_NO_FATAL_FAILURE(reload(s, one_of(build("sroot.hot", kShareRoot2, {}, 2))));
    s.world.Tick(kDt);
    EXPECT_EQ(*fld<int>(s, "got"), 3000000 + 115 * 1000 + 15);
    // A body change of the child unit.
    ASSERT_NO_FATAL_FAILURE(reload(s, one_of(build("skids.hot", kShareKids2, {{"sroot.hot", kShareRoot2}}, 3))));
    s.world.Tick(kDt);
    EXPECT_EQ(*fld<int>(s, "got"), 3000000 + 215 * 1000 + 15);
}

TEST_P(FamilyTests, StaticMembersOverTwoUnits)
{
    Session s;
    ASSERT_NO_FATAL_FAILURE(start(s,
        batch_of(build("app.hot", kStaticApp, {{"lib.hot", kStaticLib1}}, 1), build("lib.hot", kStaticLib1, {}, 1)),
        "Hero"));
    s.world.Tick(kDt);
    EXPECT_EQ(*fld<int>(s, "got"), 111);
    EXPECT_EQ(*fld<int>(s, "seen"), 111);
    EXPECT_EQ(*fld<int>(s, "mark"), 8);
    EXPECT_EQ(*fld<int>(s, "twice"), 8);
    EXPECT_EQ(*fld<int>(s, "ticks"), 1);
    s.world.Tick(kDt);
    EXPECT_EQ(*fld<int>(s, "seen"), 222);
    EXPECT_EQ(*fld<int>(s, "mark"), 9);

    std::vector<hot::LoadResult> v2;
    v2.push_back(build("lib.hot", kStaticLib2, {}, 2));
    ASSERT_NO_FATAL_FAILURE(reload(s, std::move(v2))); // the commit frame runs the old code
    EXPECT_EQ(*fld<int>(s, "seen"), 333);
    s.world.Tick(kDt);
    EXPECT_EQ(*fld<int>(s, "seen"), 333 + 11 + 200) << "the value survives and the new body runs";
    EXPECT_EQ(*fld<int>(s, "mark"), 11);
    EXPECT_EQ(*fld<int>(s, "ticks"), 4);

    std::vector<hot::LoadResult> v3;
    v3.push_back(build("lib.hot", kStaticLib3, {}, 3));
    ASSERT_NO_FATAL_FAILURE(reload(s, std::move(v3)));
    EXPECT_EQ(*fld<int>(s, "mark"), 12);
    s.world.Tick(kDt);
    EXPECT_EQ(*fld<int>(s, "mark"), 1) << "the new type resets 'mark_' to 2.5";
    EXPECT_EQ(*fld<int>(s, "seen"), 544 + 211 + 211);
    EXPECT_EQ(*fld<int>(s, "ticks"), 6);
}

// G17: overloads over two units. "ovlib.hot" has overloads of a free function, of a static method
// and of a virtual method. The importer calls each one, and "base." calls the parent overload.
const char* kOvLib1 = R"(
using "Actor.h";

int Half(int v) { return v / 2; }
int Half(std.string s) { return 70; }

class Calc {
    static int Twice(int v) { return v * 2; }
    static int Twice(float v) { return 500; }
}

class OvParent : Actor {
    virtual int Pick(int x) { return 1; }
    virtual int Pick(std.string s) { return 2; }
}
)";

// Generation 2: new bodies. The interface stays, so the importer stays.
const char* kOvLib2 = R"(
using "Actor.h";

int Half(int v) { return v / 2 + 1; }
int Half(std.string s) { return 80; }

class Calc {
    static int Twice(int v) { return v * 3; }
    static int Twice(float v) { return 600; }
}

class OvParent : Actor {
    virtual int Pick(int x) { return 3; }
    virtual int Pick(std.string s) { return 4; }
}
)";

const char* kOvApp = R"(
using "Actor.h";
using "ovlib.hot";

class OvUser : OvParent {
    int free = 0;
    int stat = 0;
    int based = 0;
    int ticks = 0;

    override int Pick(int x) { return base.Pick(x) + 10; }
    override int Pick(std.string s) { return base.Pick(s) + 20; }

    override void Tick(float dt) {
        this.ticks = this.ticks + 1;
        float f = 0.5;
        this.free = Half(8) + Half("ab");
        this.stat = Calc.Twice(3) + Calc.Twice(f);
        this.based = this.Pick(1) + this.Pick("a");
    }
}
)";

TEST_P(FamilyTests, OverloadsOverTwoUnits)
{
    Session s;
    ASSERT_NO_FATAL_FAILURE(start(s,
        batch_of(build("app.hot", kOvApp, {{"ovlib.hot", kOvLib1}}, 1), build("ovlib.hot", kOvLib1, {}, 1)),
        "OvUser"));
    s.world.Tick(kDt);
    EXPECT_EQ(*fld<int>(s, "free"), 4 + 70);
    EXPECT_EQ(*fld<int>(s, "stat"), 6 + 500);
    EXPECT_EQ(*fld<int>(s, "based"), (1 + 10) + (2 + 20));

    std::vector<hot::LoadResult> v2;
    v2.push_back(build("ovlib.hot", kOvLib2, {}, 2));
    ASSERT_NO_FATAL_FAILURE(reload(s, std::move(v2)));
    s.world.Tick(kDt);
    EXPECT_EQ(*fld<int>(s, "ticks"), 3) << "the live object keeps its state";
    EXPECT_EQ(*fld<int>(s, "free"), 5 + 80);
    EXPECT_EQ(*fld<int>(s, "stat"), 9 + 600);
    EXPECT_EQ(*fld<int>(s, "based"), (3 + 10) + (4 + 20));
}

// A missing parent module is a translation error at the "using" line.
TEST_P(BackendTests, FamilyOverTwoModulesMissingParent)
{
    hot::LoadResult r = backend->load(test_support::source("xmod_child_v1.hot"), 1);
    ASSERT_FALSE(r);
    EXPECT_NE(r.error.find("the script 'xmod_parent.hot' is not found"), std::string::npos) << r.error;
}

// Constructor overloads, a "ref" constructor parameter and parent overloads from "base(...)".
// Generation 2 uses "new" for a class without a constructor and without "new" in generation 1.
TEST_P(BackendTests, ConstructorOverloadsAndALaterNew)
{
    hot::LoadResult g1 = backend->load(test_support::source("ctor_overload_v1.hot"), 1);
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
    EXPECT_EQ(*fld<int>(s, "elite"), 309); // Elite() selects Unit(float), Elite(8) selects Unit(int)
    hot::LoadResult g2 = backend->load(test_support::source("ctor_overload_v2.hot"), 2);
    ASSERT_NO_FATAL_FAILURE(reload(s, g2, 2));
    s.world.Tick(kDt);
    EXPECT_EQ(*fld<int>(s, "plain"), 3);
}

// A "base(...)" argument binds to a "ref" or "out" parameter of a script parent (S23). The
// "ref" reaches the parent through two levels. A "&&" constructor parameter takes a temporary.
TEST_P(BackendTests, SuperArgumentBindsToARefParameter)
{
    hot::LoadResult g1 = backend->load(test_support::source("ctor_ref_v1.hot"), 1);
    ASSERT_TRUE(g1) << g1.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*g1.module), "Spawner"));
    s.commit_at_frame_boundary();
    s.world.Tick(kDt);
    EXPECT_EQ(*fld<int>(s, "mid_hp"), 5);
    EXPECT_EQ(*fld<int>(s, "seen"), 10); // Base wrote 5 * 2 into the "ref" of Mid
    EXPECT_EQ(*fld<int>(s, "leaf_hp"), 12);
    EXPECT_EQ(*fld<int>(s, "leaf_seen"), 24);
    EXPECT_EQ(*fld<int>(s, "made"), 100); // "out" through Wrap
    EXPECT_EQ(*fld<std::string>(s, "named"), "abc");
}

// S27: an overload added in a later generation has a new key type (the types of the "base(...)"
// arguments). The generation makes a new core module, and its "construct" becomes canonical.
// Generation 3 changes nothing in the keys, so it makes no new core.
TEST_P(BackendTests, ConstructorOverloadWithANewKeyTypeInALaterGeneration)
{
    hot::LoadResult g1 = backend->load(test_support::source("ctor_newkey_v1.hot"), 1);
    ASSERT_TRUE(g1) << g1.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*g1.module), "Spawner"));
    s.commit_at_frame_boundary();
    s.world.Tick(kDt);
    EXPECT_EQ(*fld<int>(s, "hp_int"), 3);
    EXPECT_EQ(*fld<int>(s, "hp_float"), 0);
    const size_t cores1 = s.loader.stats().core_modules;

    hot::LoadResult g2 = backend->load(test_support::source("ctor_newkey_v2.hot"), 2);
    ASSERT_NO_FATAL_FAILURE(reload(s, g2, 2));
    s.world.Tick(kDt);
    EXPECT_EQ(*fld<int>(s, "hp_int"), 3);
    EXPECT_EQ(*fld<int>(s, "hp_float"), 40); // the new overload ran
    EXPECT_EQ(*fld<int>(s, "kind_float"), 2);
    EXPECT_EQ(s.loader.stats().core_modules, cores1 + 1);
    // The object of the new key uses the canonical record: a later reload reaches it.
    hot::Instance made = s.loader.find("Unit")->create();
    EXPECT_EQ(made.shell->cls, s.loader.find("Unit")->record);
    delete static_cast<Actor*>(made.native);

    hot::LoadResult g3 = backend->load(test_support::source("ctor_newkey_v2.hot"), 3);
    ASSERT_NO_FATAL_FAILURE(reload(s, g3, 3));
    s.world.Tick(kDt);
    EXPECT_EQ(*fld<int>(s, "hp_float"), 40);
    EXPECT_EQ(s.loader.stats().core_modules, cores1 + 1);
}

// The parent unit "priv.hot": the public class "Shown" derives from a private class.
const char* const kPriv1 = R"(
using "Actor.h";
private class Hidden : Actor {
    int a = 1;
}
class Shown : Hidden {
    int b = 2;
}
)";

// The child unit "kid.hot": it derives from "Shown" and adds a field after the parent state.
const char* const kPrivKid = R"(
using "Actor.h";
using "priv.hot";
class Kid : Shown {
    int z = 7;
}
)";

// Requests a reload of "unit" alone and expects the rejection DependentsStale for "dependent".
void expect_stale(Session& s, hot::LoadResult r, const std::string& dependent)
{
    ASSERT_TRUE(r) << r.error;
    std::vector<hot::LoadResult> alone;
    alone.push_back(std::move(r));
    const hot::ReloadResult stale = s.loader.request_reload(std::move(alone));
    EXPECT_EQ(stale.status, hot::ReloadStatus::Failed);
    EXPECT_EQ(stale.reason, hot::ReloadReason::DependentsStale) << stale.error;
    EXPECT_EQ(stale.units, std::vector<std::string>{dependent});
    EXPECT_FALSE(s.loader.reload_pending());
}

// A new field of a private parent changes the state layout of the public class. The live child
// in another unit compiled against the old layout: a reload of the parent unit alone is rejected.
TEST_P(FamilyTests, PrivateParentChangeNeedsTheChildInTheBatch)
{
    Session s;
    ASSERT_NO_FATAL_FAILURE(start(s,
        batch_of(build("kid.hot", kPrivKid, {{"priv.hot", kPriv1}}, 1), build("priv.hot", kPriv1, {}, 1)), "Kid"));
    std::string p2 = kPriv1;
    p2.replace(p2.find("int a = 1;"), 10, "int a = 1;\n    int c = 3;");
    ASSERT_NO_FATAL_FAILURE(expect_stale(s, build("priv.hot", p2, {}, 2), "kid"));
    EXPECT_EQ(*fld<int>(s, "z"), 7);
}

// The loader checks the parent layout of a live child outside the batch, also when the interface
// hash of the parent unit stays the same. The test gives the new module the old hash.
TEST_P(FamilyTests, ParentLayoutChangeWithTheSameHashNeedsTheChildInTheBatch)
{
    Session s;
    hot::LoadResult p1 = build("priv.hot", kPriv1, {}, 1);
    ASSERT_TRUE(p1) << p1.error;
    const uint64_t hash = p1.iface_hash;
    ASSERT_NO_FATAL_FAILURE(start(s, batch_of(build("kid.hot", kPrivKid, {{"priv.hot", kPriv1}}, 1), std::move(p1)), "Kid"));
    std::string text = kPriv1;
    text.replace(text.find("int b = 2;"), 10, "int b = 2;\n    int d = 4;");
    hot::LoadResult p2 = build("priv.hot", text, {}, 2);
    ASSERT_TRUE(p2) << p2.error;
    p2.iface_hash = hash;
    p2.module->iface_hash = hash;
    ASSERT_NO_FATAL_FAILURE(expect_stale(s, std::move(p2), "kid"));
    EXPECT_EQ(*fld<int>(s, "z"), 7);
}

// An importer has a copy of each struct with the method bodies. A body change of a struct changes
// the interface: a reload of the owner unit alone is rejected.
TEST_P(FamilyTests, StructBodyChangeNeedsTheImporterInTheBatch)
{
    const std::string pt1 = "struct Pt {\n    int v = 1;\n    int Get() { return this.v + 1; }\n}\n";
    const std::string user = R"(
using "Actor.h";
using "pt.hot";
class User : Actor {
    Pt p;
    int g = 0;
    override void Tick(float dt) { this.g = this.p.Get(); }
}
)";
    Session s;
    ASSERT_NO_FATAL_FAILURE(start(s, batch_of(build("user.hot", user, {{"pt.hot", pt1}}, 1), build("pt.hot", pt1, {}, 1)), "User"));
    s.world.Tick(kDt);
    EXPECT_EQ(*fld<int>(s, "g"), 2);
    std::string pt2 = pt1;
    pt2.replace(pt2.find("+ 1"), 3, "+ 5");
    ASSERT_NO_FATAL_FAILURE(expect_stale(s, build("pt.hot", pt2, {}, 2), "user"));
}

std::string kind_name(const ::testing::TestParamInfo<hot::BackendKind>& info)
{
    return info.param == hot::BackendKind::Jit ? "Jit" : "ClangFiles";
}

INSTANTIATE_TEST_SUITE_P(
    Backends, FamilyTests, ::testing::Values(hot::BackendKind::Jit, hot::BackendKind::ClangFiles), kind_name);

} // namespace
