// Tests of the result of a reload that hot reload cannot apply (full reload, steps 1-3):
// the status, the reason and the classes of a ReloadResult, and the deferred write of the
// slot state. The slot state changes only when the Loader accepts the module. The last tests
// run the full sequence: reset of the Loader, a fresh load and a later hot reload.
#include "hot/backend.h"
#include "support/backend_fixture.h"
#include "support/reload_assert.h"
#include "support/script_files.h"
#include "support/test_support.h"

#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifdef HOT_TEST_CLANG_FILES
    #include "hot/clang_files_backend.h"
#endif

namespace
{

using test_support::kDt;
using test_support::Session;
using test_support::source;

#ifdef HOT_TEST_CLANG_FILES
std::string slots_text(hot::Backend& backend)
{
    auto* files = dynamic_cast<hot::ClangFilesBackend*>(&backend);
    if (!files)
        return "<not the file backend>";
    std::ifstream f(files->slots_file());
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
#endif

// A change of the script parent needs a full reload. The result names the class.
TEST_P(BackendTests, ParentChangeRequiresFullReload)
{
    hot::LoadResult v1 = backend->load(source("family_v1.hot"), 1);
    ASSERT_TRUE(v1) << v1.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Driver"));
    s.commit_at_frame_boundary();
    hot::LoadResult bad = backend->load(source("family_reparent.hot"), 2);
    ASSERT_TRUE(bad) << bad.error;

    const hot::ReloadResult res = s.loader.request_reload(std::move(*bad.module));
    EXPECT_FALSE(res);
    EXPECT_EQ(res.status, hot::ReloadStatus::FullReloadRequired);
    EXPECT_EQ(res.reason, hot::ReloadReason::ScriptParentChanged);
    EXPECT_EQ(res.classes, std::vector<std::string>{"G"});
    EXPECT_NE(res.error.find("script parent class changed"), std::string::npos) << res.error;
    EXPECT_FALSE(s.loader.reload_pending());
    EXPECT_EQ(hot::active_generation, 1u);
}

// A reload that the Loader rejects at run time leaves the slots file as it was. The backend
// wrote the file at the end of the build before; now the commit of an accepted module writes it.
TEST_P(BackendTests, RejectedReloadLeavesTheSlotsFileUnchanged)
{
#ifdef HOT_TEST_CLANG_FILES
    hot::LoadResult v1 = backend->load(source("family_v1.hot"), 1);
    ASSERT_TRUE(v1) << v1.error;
    // The build alone does not write the file.
    EXPECT_TRUE(slots_text(*backend).empty());
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Driver"));
    s.commit_at_frame_boundary();
    const std::string after_v1 = slots_text(*backend);
    EXPECT_FALSE(after_v1.empty()); // the Loader accepted generation 1

    hot::LoadResult bad = backend->load(source("family_reparent.hot"), 2);
    ASSERT_TRUE(bad) << bad.error;
    EXPECT_EQ(slots_text(*backend), after_v1); // the build did not write
    const hot::ReloadResult res = s.loader.request_reload(std::move(*bad.module));
    EXPECT_EQ(res.status, hot::ReloadStatus::FullReloadRequired);
    EXPECT_EQ(slots_text(*backend), after_v1); // the reject did not write

    // An accepted reload writes the file.
    hot::LoadResult v2 = backend->load(source("family_v2.hot"), 2);
    ASSERT_TRUE(v2) << v2.error;
    EXPECT_EQ(slots_text(*backend), after_v1);
    ASSERT_RELOAD_OK(s.loader.request_reload(std::move(*v2.module)));
    EXPECT_EQ(slots_text(*backend), after_v1); // a pending reload did not write yet
    s.world.Tick(kDt); // the frame boundary commits: the generation is live
    EXPECT_NE(slots_text(*backend), after_v1);
#else
    GTEST_SKIP() << "only the file backend has a slots file";
#endif
}

// A removed script function shrinks the slot count. sppc fails with the reason SlotsRemoved.
// The Loader overload for a LoadResult gives FullReloadRequired.
TEST_P(BackendTests, SlotsRemovalRequiresFullReload)
{
    hot::LoadResult v1 = backend->load(source("fnremoved_v1.hot"), 1);
    ASSERT_TRUE(v1) << v1.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Medic"));
    s.commit_at_frame_boundary();

    hot::LoadResult v2 = backend->load(source("fnremoved_v2.hot"), 2);
    ASSERT_FALSE(v2);
    EXPECT_EQ(v2.reason, hot::ReloadReason::SlotsRemoved);
    EXPECT_NE(v2.error.find("slot count must not shrink"), std::string::npos) << v2.error;

    const hot::ReloadResult res = s.loader.request_reload(std::move(v2));
    EXPECT_FALSE(res);
    EXPECT_EQ(res.status, hot::ReloadStatus::FullReloadRequired);
    EXPECT_EQ(res.reason, hot::ReloadReason::SlotsRemoved);
    EXPECT_FALSE(res.error.empty());
    EXPECT_FALSE(s.loader.reload_pending());
    EXPECT_EQ(hot::active_generation, 1u);
}

// A script with a compile error is a failed reload, not a full reload.
TEST_P(BackendTests, CompileErrorIsAFailedReload)
{
    // Build the bad script first: with the slot state of player_v1 it would fail for the removed
    // functions (SlotsRemoved) before it reaches the type error.
    hot::LoadResult bad = backend->load(source("bad.hot"), 1);
    ASSERT_FALSE(bad);
    EXPECT_EQ(bad.reason, hot::ReloadReason::CompileError);

    hot::LoadResult v1 = backend->load(source("player_v1.hot"), 1);
    ASSERT_TRUE(v1) << v1.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module)));
    s.commit_at_frame_boundary();

    const hot::ReloadResult res = s.loader.request_reload(std::move(bad));
    EXPECT_FALSE(res);
    EXPECT_EQ(res.status, hot::ReloadStatus::Failed);
    EXPECT_EQ(res.reason, hot::ReloadReason::CompileError);
    EXPECT_FALSE(res.error.empty());
    EXPECT_FALSE(s.loader.reload_pending());
}

// A second request while one is pending is Busy. A first load that works is Applied.
TEST_P(BackendTests, BusyAndAppliedResults)
{
    hot::LoadResult v1 = backend->load(source("player_v1.hot"), 1);
    ASSERT_TRUE(v1) << v1.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module)));
    hot::LoadResult v2 = backend->load(source("player_v2.hot"), 2);
    ASSERT_TRUE(v2) << v2.error;
    const hot::ReloadResult first = s.loader.request_reload(std::move(*v2.module));
    EXPECT_EQ(first.status, hot::ReloadStatus::Pending);
    EXPECT_TRUE(first);

    hot::LoadResult v3 = backend->load(source("player_v2.hot"), 3);
    ASSERT_TRUE(v3) << v3.error;
    const hot::ReloadResult second = s.loader.request_reload(std::move(*v3.module));
    EXPECT_FALSE(second);
    EXPECT_EQ(second.status, hot::ReloadStatus::Failed);
    EXPECT_EQ(second.reason, hot::ReloadReason::Busy);

    const hot::ReloadResult committed = s.loader.commit_pending();
    EXPECT_EQ(committed.status, hot::ReloadStatus::Applied);
    EXPECT_TRUE(committed);
    // Nothing is pending now.
    const hot::ReloadResult none = s.loader.commit_pending();
    EXPECT_FALSE(none);
    EXPECT_EQ(none.reason, hot::ReloadReason::None);
}

// A fresh load ignores the slot state of the earlier generations: the removed function is no
// error. It builds a new library, so it never overwrites a mapped one.
TEST_P(BackendTests, FreshLoadIgnoresTheSlotState)
{
    hot::LoadResult v1 = backend->load(source("fnremoved_v1.hot"), 1);
    ASSERT_TRUE(v1) << v1.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Medic"));
    s.commit_at_frame_boundary();

#ifdef HOT_TEST_CLANG_FILES
    const std::string before = slots_text(*backend);
    EXPECT_FALSE(before.empty());
#endif
    hot::LoadResult fresh = backend->load(source("fnremoved_v2.hot"), 1, hot::LoadMode::Fresh);
    ASSERT_TRUE(fresh) << fresh.error;
    EXPECT_FALSE(fresh.module->origin.empty());
#ifdef HOT_TEST_CLANG_FILES
    EXPECT_EQ(slots_text(*backend), before); // the build did not write
#endif
}

// A host with a Loader and one script object that it owns. The host frees the object before the
// Loader goes. "drop" is the step "drop all script handles" of a full reload.
struct FullReloadHost
{
    hot::Loader loader;
    Actor* actor = nullptr;
    hot::ShellBase* shell = nullptr;

    void create(const char* class_name)
    {
        const hot::ClassEntry* entry = loader.find(class_name);
        ASSERT_NE(entry, nullptr) << class_name;
        const hot::Instance inst = entry->create();
        actor = static_cast<Actor*>(inst.native);
        shell = inst.shell;
    }
    template <class T> T* field(const char* name) { return hot::field_ptr<T>(shell, name); }
    void drop()
    {
        delete actor;
        actor = nullptr;
        shell = nullptr;
    }
    ~FullReloadHost() { drop(); }
};

// The full sequence for a change of the script parent: save a value, drop the handles, reset,
// load the fresh module as generation 1, restore the value. Then a hot reload works again.
TEST_P(BackendTests, FullReloadSequenceAfterAParentChange)
{
    hot::LoadResult v1 = backend->load(source("family_v1.hot"), 1);
    ASSERT_TRUE(v1) << v1.error;
    FullReloadHost host;
    ASSERT_RELOAD_OK(host.loader.load(std::move(*v1.module)));
    ASSERT_NO_FATAL_FAILURE(host.create("Driver"));
    host.actor->Tick(kDt);
    host.actor->Tick(kDt);
    ASSERT_EQ(*host.field<int>("ticks"), 2);
    ASSERT_EQ(*host.field<int>("leaf_score"), 21);

    hot::LoadResult v2 = backend->load(source("family_reparent.hot"), 2);
    ASSERT_TRUE(v2) << v2.error;
    const hot::ReloadResult res = host.loader.request_reload(std::move(*v2.module));
    ASSERT_EQ(res.status, hot::ReloadStatus::FullReloadRequired) << res.error;
    EXPECT_EQ(res.reason, hot::ReloadReason::ScriptParentChanged);

    // (a) The host saves its own state.
    const int saved_ticks = *host.field<int>("ticks");

    // The Driver holds a G and an S: the reset names them.
    std::string error;
    EXPECT_FALSE(host.loader.reset(error));
    EXPECT_EQ(error, "3 live objects remain: Driver x1, G x1, S x1");
    EXPECT_EQ(host.loader.stats().active_generation, 1u); // the failed reset changed nothing

    // (b) The host drops the handles. (c) The reset works now.
    host.drop();
    ASSERT_TRUE(host.loader.reset(error)) << error;
    const hot::LoaderStats cleared = host.loader.stats();
    EXPECT_EQ(cleared.active_generation, 0u);
    EXPECT_TRUE(cleared.generations.empty());
    EXPECT_TRUE(cleared.classes.empty());
    EXPECT_EQ(hot::active_generation, 0u);

    // (d) The fresh load. (e) The Loader takes it as generation 1.
    hot::LoadResult fresh = backend->load(source("family_reparent.hot"), 1, hot::LoadMode::Fresh);
    ASSERT_TRUE(fresh) << fresh.error;
    ASSERT_RELOAD_OK(host.loader.load(std::move(*fresh.module)));
    EXPECT_EQ(hot::active_generation, 1u);
    ASSERT_EQ(host.loader.stats().generations.size(), 1u);

    // (f) The host restores its state. The new classes work.
    ASSERT_NO_FATAL_FAILURE(host.create("Driver"));
    *host.field<int>("ticks") = saved_ticks;
    host.actor->Tick(kDt);
    EXPECT_EQ(*host.field<int>("ticks"), saved_ticks + 1);
    EXPECT_EQ(*host.field<int>("leaf_score"), 21);
    EXPECT_EQ(*host.field<int>("sib_score"), 7);
    EXPECT_EQ(*host.field<int>("top_f"), 2); // G.f: A.f(1) * 2 (G is a child of S now)
    EXPECT_EQ(*host.field<int>("leaf_a"), 3); // G() : base() calls S() : base(3), then A(3)

#ifdef HOT_TEST_CLANG_FILES
    // The slots file has the slots of the fresh generation only: S got its own h().
    const std::string slots = slots_text(*backend);
    EXPECT_NE(slots.find("S::h"), std::string::npos) << slots;
#endif

    // A later hot reload works after the full reload. Generation 2 is compatible with generation 1.
    hot::LoadResult v3 = backend->load(source("family_reparent_v2.hot"), 2);
    ASSERT_TRUE(v3) << v3.error;
    ASSERT_RELOAD_OK(host.loader.request_reload(std::move(*v3.module)));
    ASSERT_RELOAD_OK(host.loader.commit_pending());
    EXPECT_EQ(hot::active_generation, 2u);
    host.actor->Tick(kDt);
    EXPECT_EQ(*host.field<int>("leaf_rank"), 6); // the new property Rank: 3 for the child and the sibling
    EXPECT_EQ(*host.field<int>("ticks"), saved_ticks + 2);
}

// A removed function needs a full reload. The fresh slots file has no entry of the old function.
TEST_P(BackendTests, FullReloadSequenceAfterARemovedFunction)
{
    hot::LoadResult v1 = backend->load(source("fnremoved_v1.hot"), 1);
    ASSERT_TRUE(v1) << v1.error;
    FullReloadHost host;
    ASSERT_RELOAD_OK(host.loader.load(std::move(*v1.module)));
    ASSERT_NO_FATAL_FAILURE(host.create("Medic"));
    *host.field<int>("hp") = 42;
#ifdef HOT_TEST_CLANG_FILES
    EXPECT_NE(slots_text(*backend).find("Medic::rest"), std::string::npos);
#endif

    hot::LoadResult v2 = backend->load(source("fnremoved_v2.hot"), 2);
    ASSERT_FALSE(v2);
    ASSERT_EQ(host.loader.request_reload(std::move(v2)).status, hot::ReloadStatus::FullReloadRequired);

    const int saved_hp = *host.field<int>("hp");
    std::string error;
    EXPECT_FALSE(host.loader.reset(error)); // the object is still held
    EXPECT_EQ(error, "1 live objects remain: Medic x1");
    host.drop();
    ASSERT_TRUE(host.loader.reset(error)) << error;

    hot::LoadResult fresh = backend->load(source("fnremoved_v2.hot"), 1, hot::LoadMode::Fresh);
    ASSERT_TRUE(fresh) << fresh.error;
    ASSERT_RELOAD_OK(host.loader.load(std::move(*fresh.module)));
    ASSERT_NO_FATAL_FAILURE(host.create("Medic"));
    *host.field<int>("hp") = saved_hp;
    EXPECT_EQ(*host.field<int>("hp"), 42);
    EXPECT_EQ(hot::active_generation, 1u);
#ifdef HOT_TEST_CLANG_FILES
    const std::string slots = slots_text(*backend);
    EXPECT_EQ(slots.find("Medic::rest"), std::string::npos) << slots;
    EXPECT_NE(slots.find("Medic::heal"), std::string::npos) << slots;
#endif
}

// A script with a module variable block: reset frees the block. The fresh load makes a new
// block. The module state is valid and has the initial values again (design 5.3).
TEST_P(BackendTests, ResetAndFreshLoadMakeANewModuleBlock)
{
    auto total = [](hot::Loader& loader) -> int*
    {
        const hot::ModuleStateInfo ms = loader.module_state();
        if (!ms.layout || !ms.slot || !*ms.slot)
            return nullptr;
        for (uint32_t i = 0; i < ms.layout->field_count; ++i)
            if (std::string(ms.layout->fields[i].name) == "total")
                return reinterpret_cast<int*>(static_cast<char*>(*ms.slot) + ms.layout->fields[i].offset);
        return nullptr;
    };
    hot::LoadResult v1 = backend->load(source("module_v1.hot"), 1);
    ASSERT_TRUE(v1) << v1.error;
    FullReloadHost host;
    ASSERT_RELOAD_OK(host.loader.load(std::move(*v1.module)));
    ASSERT_NO_FATAL_FAILURE(host.create("Hero"));
    ASSERT_NE(total(host.loader), nullptr);
    host.actor->Tick(kDt);
    EXPECT_EQ(*total(host.loader), 1); // Hero adds the weight of Walk
    host.drop();

    std::string error;
    ASSERT_TRUE(host.loader.reset(error)) << error;
    EXPECT_EQ(host.loader.module_state().layout, nullptr); // no block after the reset

    hot::LoadResult fresh = backend->load(source("module_v1.hot"), 1, hot::LoadMode::Fresh);
    ASSERT_TRUE(fresh) << fresh.error;
    ASSERT_RELOAD_OK(host.loader.load(std::move(*fresh.module)));
    ASSERT_NE(total(host.loader), nullptr); // the new block is valid
    EXPECT_EQ(*total(host.loader), 0); // with the initial value
    ASSERT_NO_FATAL_FAILURE(host.create("Hero"));
    host.actor->Tick(kDt);
    EXPECT_EQ(*total(host.loader), 1);
}

// The loader keeps one first generation: a second load needs a reset before it.
TEST_P(BackendTests, LoadFailsWhenTheLoaderHasAGeneration)
{
    hot::LoadResult v1 = backend->load(source("fnremoved_v1.hot"), 1);
    ASSERT_TRUE(v1) << v1.error;
    hot::LoadResult again = backend->load(source("fnremoved_v2.hot"), 1, hot::LoadMode::Fresh);
    ASSERT_TRUE(again) << again.error;
    FullReloadHost host;
    ASSERT_RELOAD_OK(host.loader.load(std::move(*v1.module)));
    const hot::ReloadResult r = host.loader.load(std::move(*again.module));
    EXPECT_EQ(r.status, hot::ReloadStatus::Failed);
    EXPECT_EQ(r.reason, hot::ReloadReason::BadModule);
    EXPECT_EQ(r.error, "the loader already has a generation: call reset first");
}

// A module variable can hold a closure inside std.function. The closure pins its generation,
// so reset fails with "generation 1 is pinned" until a script function clears the variable.
// The host must call such a function before reset (doc/hot-reload.md).
TEST_P(BackendTests, ModuleVariableWithAClosurePinsTheGeneration)
{
    hot::LoadResult v1 = backend->load(source("modclosure.hot"), 1);
    ASSERT_TRUE(v1) << v1.error;
    FullReloadHost host;
    ASSERT_RELOAD_OK(host.loader.load(std::move(*v1.module)));
    ASSERT_NO_FATAL_FAILURE(host.create("Holder"));
    auto store = reinterpret_cast<void (*)(Actor*)>(host.loader.thunk("Holder::Store"));
    auto clear = reinterpret_cast<void (*)(Actor*)>(host.loader.thunk("Holder::Clear"));
    ASSERT_NE(store, nullptr);
    ASSERT_NE(clear, nullptr);
    store(host.actor);
    host.drop();
    std::string error;
    EXPECT_FALSE(host.loader.reset(error));
    EXPECT_EQ(error, "generation 1 is pinned");
    ASSERT_NO_FATAL_FAILURE(host.create("Holder"));
    clear(host.actor); // the script function releases the closure
    host.drop();
    EXPECT_TRUE(host.loader.reset(error)) << error;
}

} // namespace
