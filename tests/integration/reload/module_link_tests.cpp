// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Reload tests of two script units that the loader links at run time (modules step 4). The unit
// "a.hot" imports the unit "b.hot". Each unit is its own module. A batch reload commits several
// units at once. The tests run on the JIT backend and on the clang files backend.
#include "hot/backend.h"
#include "support/reload_assert.h"
#include "support/test_support.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace
{

using test_support::kDt;
using test_support::Session;

// The module B: a module variable, a free function and a class.
const char* const kB1 = R"(
int counter = 0;
int twice(int v) { return v * 2; }
class Box {
    int size = 1;
    Box() { }
    Box(int s) { this.size = s; }
    int area() { return this.size * this.size; }
    void grow(int by) { this.size = this.size + by; }
}
)";

// A body change of B: the interface stays the same.
const char* const kB2 = R"(
int counter = 0;
int twice(int v) { return v * 3; }
class Box {
    int size = 1;
    Box() { }
    Box(int s) { this.size = s; }
    int area() { return this.size * this.size + 1000; }
    void grow(int by) { this.size = this.size + by * 2; }
}
)";

// A field change of B: the interface changes.
const char* const kB3 = R"(
int counter = 0;
int twice(int v) { return v * 2; }
class Box {
    int extra = 5;
    int size = 1;
    Box() { }
    Box(int s) { this.size = s; }
    int area() { return this.size * this.size + this.extra; }
    void grow(int by) { this.size = this.size + by; }
}
)";

// The module A: it uses each part of B.
const char* const kA = R"(
using "Actor.h";
using "b.hot";
class Player : Actor {
    $Box box;
    int ticks = 0;
    int last = 0;
    int tw = 0;
    int sz = 0;
    int seen = 0;
    override void Tick(float dt) {
        $Box b = this.box;
        if (b == null) {
            b = new $Box(3);
            this.box = b;
        }
        this.ticks = this.ticks + 1;
        this.last = b.area();
        this.tw = twice(this.ticks);
        this.sz = b.size;
        counter = counter + 1;
        this.seen = counter;
    }
}
)";

// A cycle: A2 imports B4, and B4 imports A2.
const char* const kA2 = R"(
using "Actor.h";
using "b.hot";
int afn(int v) { return v * 10; }
class Player : Actor {
    int c = 0;
    override void Tick(float dt) { this.c = bfn(2); }
}
)";

const char* const kB4 = R"(
using "a.hot";
int bfn(int v) { return afn(v) + 1; }
)";

const char* const kB5 = R"(
using "a.hot";
int bfn(int v) { return afn(v) + 2; }
)";

class ModuleLinkTests : public ::testing::TestWithParam<hot::BackendKind>
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

    // Builds one unit. "dep" is the text of the other unit, under its import name.
    hot::LoadResult build(const char* unit, const char* text, const char* dep_name, const char* dep, uint32_t gen)
    {
        hot::ScriptSource src{unit, text, {}};
        if (dep_name)
            src.modules.emplace_back(dep_name, dep);
        hot::LoadResult r = backend->load(src, gen);
        if (r)
            r.module->unit = unit;
        return r;
    }

    // Loads the units as one batch and makes a Player.
    void start(Session& s, std::vector<hot::LoadResult> units)
    {
        std::vector<hot::Module> modules;
        for (hot::LoadResult& r : units)
        {
            ASSERT_TRUE(r) << r.error;
            modules.push_back(std::move(*r.module));
        }
        ASSERT_RELOAD_OK(s.loader.load(std::move(modules)));
        const hot::ClassEntry* e = s.loader.find("Player");
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

int fld(Session& s, const char* name)
{
    return *hot::field_ptr<int>(s.inst.shell, name);
}

// A body change of B reloads B alone. A keeps its code and its object state. The module variable of
// B, which A writes, keeps its value.
TEST_P(ModuleLinkTests, BodyChangeOfTheImportKeepsTheImporter)
{
    hot::LoadResult b1 = build("b.hot", kB1, nullptr, nullptr, 1);
    hot::LoadResult a1 = build("a.hot", kA, "b.hot", kB1, 1);
    ASSERT_TRUE(a1) << a1.error;
    ASSERT_EQ(a1.import_hashes.size(), 1u);
    EXPECT_EQ(a1.import_hashes[0].first, "b.hot");
    EXPECT_EQ(a1.import_hashes[0].second, b1.iface_hash);
    Session s;
    std::vector<hot::LoadResult> first;
    first.push_back(std::move(a1)); // the importer first: the loader orders the batch
    first.push_back(std::move(b1));
    ASSERT_NO_FATAL_FAILURE(start(s, std::move(first)));
    s.world.Tick(kDt);
    s.world.Tick(kDt);
    EXPECT_EQ(fld(s, "ticks"), 2);
    EXPECT_EQ(fld(s, "last"), 9);
    EXPECT_EQ(fld(s, "tw"), 4);
    EXPECT_EQ(fld(s, "sz"), 3);
    EXPECT_EQ(fld(s, "seen"), 2);
    const hot::ClassRecord* player = s.inst.shell->cls;
    const uint32_t player_gen = player->generation;

    hot::LoadResult b2 = build("b.hot", kB2, nullptr, nullptr, 2);
    ASSERT_TRUE(b2) << b2.error;
    EXPECT_EQ(b2.iface_hash, b1.iface_hash) << "a body change keeps the interface hash";
    std::vector<hot::LoadResult> batch;
    batch.push_back(std::move(b2));
    ASSERT_NO_FATAL_FAILURE(reload(s, std::move(batch)));
    s.world.Tick(kDt);
    EXPECT_EQ(s.inst.shell->cls, player);
    EXPECT_EQ(player->generation, player_gen) << "A is not in the batch";
    EXPECT_EQ(fld(s, "ticks"), 4); // one tick in the commit frame, one after it
    EXPECT_EQ(fld(s, "last"), 1009); // the new body of Box.area
    EXPECT_EQ(fld(s, "tw"), 12); // the new body of twice
    EXPECT_EQ(fld(s, "seen"), 4); // the module variable of B survived the reload
}

// A field change of B changes its interface. A reload of B alone is rejected, because the live A
// compiled against the old interface. A batch of B and the rebuilt A commits. The objects keep
// their state.
TEST_P(ModuleLinkTests, FieldChangeOfTheImportNeedsTheImporterInTheBatch)
{
    hot::LoadResult b1 = build("b.hot", kB1, nullptr, nullptr, 1);
    hot::LoadResult a1 = build("a.hot", kA, "b.hot", kB1, 1);
    Session s;
    std::vector<hot::LoadResult> first;
    first.push_back(std::move(b1));
    first.push_back(std::move(a1));
    ASSERT_NO_FATAL_FAILURE(start(s, std::move(first)));
    s.world.Tick(kDt);
    EXPECT_EQ(fld(s, "last"), 9);

    hot::LoadResult b3 = build("b.hot", kB3, nullptr, nullptr, 2);
    ASSERT_TRUE(b3) << b3.error;
    std::vector<hot::LoadResult> alone;
    alone.push_back(std::move(b3));
    const hot::ReloadResult stale = s.loader.request_reload(std::move(alone));
    EXPECT_EQ(stale.status, hot::ReloadStatus::Failed);
    EXPECT_EQ(stale.reason, hot::ReloadReason::DependentsStale) << stale.error;
    EXPECT_EQ(stale.units, std::vector<std::string>{"a.hot"});
    EXPECT_FALSE(s.loader.reload_pending());

    // A new generation number: the rejected build keeps its library name.
    std::vector<hot::LoadResult> batch;
    batch.push_back(build("b.hot", kB3, nullptr, nullptr, 3));
    batch.push_back(build("a.hot", kA, "b.hot", kB3, 3));
    ASSERT_NO_FATAL_FAILURE(reload(s, std::move(batch)));
    s.world.Tick(kDt);
    EXPECT_EQ(fld(s, "ticks"), 3);
    EXPECT_EQ(fld(s, "sz"), 3); // the Box object kept its size
    EXPECT_EQ(fld(s, "last"), 14); // 3 * 3 + the new field "extra" (5)
    EXPECT_EQ(fld(s, "seen"), 3);
    EXPECT_EQ(s.inst.shell->cls->generation, 3u);
}

// An importer that compiled against another interface of a live unit is rejected.
TEST_P(ModuleLinkTests, ImportWithAnotherHashIsRejected)
{
    Session s;
    std::vector<hot::LoadResult> first;
    first.push_back(build("b.hot", kB1, nullptr, nullptr, 1));
    first.push_back(build("a.hot", kA, "b.hot", kB1, 1));
    ASSERT_NO_FATAL_FAILURE(start(s, std::move(first)));
    s.world.Tick(kDt);
    std::vector<hot::LoadResult> batch;
    batch.push_back(build("a.hot", kA, "b.hot", kB3, 2)); // the live B is kB1
    const hot::ReloadResult r = s.loader.request_reload(std::move(batch));
    EXPECT_EQ(r.status, hot::ReloadStatus::Failed);
    EXPECT_EQ(r.reason, hot::ReloadReason::ImportMismatch) << r.error;
    EXPECT_EQ(r.units, std::vector<std::string>{"a.hot"});
    // A first load also checks the imports.
    Session t;
    std::vector<hot::Module> mods;
    hot::LoadResult a = build("a.hot", kA, "b.hot", kB3, 1);
    ASSERT_TRUE(a) << a.error;
    mods.push_back(std::move(*a.module));
    const hot::ReloadResult l = t.loader.load(std::move(mods));
    EXPECT_EQ(l.reason, hot::ReloadReason::ImportMismatch) << l.error;
    EXPECT_TRUE(t.loader.stats().generations.empty());
}

// A imports B and B imports A. Both load in one batch, and a body change of B reloads B alone.
TEST_P(ModuleLinkTests, ImportCycle)
{
    hot::LoadResult a = build("a.hot", kA2, "b.hot", kB4, 1);
    hot::LoadResult b = build("b.hot", kB4, "a.hot", kA2, 1);
    ASSERT_TRUE(a) << a.error;
    ASSERT_TRUE(b) << b.error;
    ASSERT_EQ(b.import_hashes.size(), 1u);
    EXPECT_EQ(b.import_hashes[0].second, a.iface_hash);
    Session s;
    std::vector<hot::LoadResult> first;
    first.push_back(std::move(a));
    first.push_back(std::move(b));
    ASSERT_NO_FATAL_FAILURE(start(s, std::move(first)));
    s.world.Tick(kDt);
    EXPECT_EQ(fld(s, "c"), 21);
    std::vector<hot::LoadResult> batch;
    batch.push_back(build("b.hot", kB5, "a.hot", kA2, 2));
    ASSERT_NO_FATAL_FAILURE(reload(s, std::move(batch)));
    s.world.Tick(kDt);
    EXPECT_EQ(fld(s, "c"), 22);
}

std::string kind_name(const ::testing::TestParamInfo<hot::BackendKind>& info)
{
    return info.param == hot::BackendKind::Jit ? "Jit" : "ClangFiles";
}

INSTANTIATE_TEST_SUITE_P(
    Backends, ModuleLinkTests, ::testing::Values(hot::BackendKind::Jit, hot::BackendKind::ClangFiles), kind_name);

} // namespace
