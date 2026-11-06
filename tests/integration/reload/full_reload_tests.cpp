// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

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

#ifdef HOT_WITH_CLANG_FILES
    #include "hot/clang_files_backend.h"
#endif

namespace
{

using test_support::kDt;
using test_support::Session;
using test_support::source;

// True when the default backend is the clang files backend (the environment variable HOT_BACKEND
// can choose the JIT backend). Only this backend has a slots file.
bool is_files_backend(hot::Backend& backend)
{
    return std::string(backend.name()) == "CLANG_FILES";
}

std::string slots_text([[maybe_unused]] hot::Backend& backend)
{
#ifdef HOT_WITH_CLANG_FILES
    if (auto* files = dynamic_cast<hot::ClangFilesBackend*>(&backend))
    {
        std::ifstream f(files->slots_file());
        std::stringstream ss;
        ss << f.rdbuf();
        return ss.str();
    }
#endif
    return "<not the file backend>";
}

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
    if (!is_files_backend(*backend))
        GTEST_SKIP() << "Only the clang files backend has a slots file.";
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
}

// A removed script function keeps its slot as a defunct entry. The reload applies live. The
// function that comes back with a new signature reuses the slot and gets a keyed thunk.
TEST_P(BackendTests, RemovedFunctionReloadsLive)
{
    hot::LoadResult v1 = backend->load(source("fnremoved_v1.hot"), 1);
    ASSERT_TRUE(v1) << v1.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Medic"));
    s.commit_at_frame_boundary();

    hot::LoadResult v2 = backend->load(source("fnremoved_v2.hot"), 2);
    ASSERT_TRUE(v2) << v2.error;
    EXPECT_EQ(v2.reason, hot::ReloadReason::None);
    ASSERT_RELOAD_OK(s.loader.request_reload(std::move(*v2.module)));
    s.world.Tick(kDt); // the frame boundary commits: the generation is live
    EXPECT_EQ(hot::active_generation, 2u);
    if (is_files_backend(*backend))
        EXPECT_NE(slots_text(*backend).find("Medic::rest 1"), std::string::npos) << slots_text(*backend);

    hot::LoadResult v3 = backend->load(source("fnremoved_v3.hot"), 3);
    ASSERT_TRUE(v3) << v3.error;
    EXPECT_EQ(v3.reason, hot::ReloadReason::None);
    ASSERT_RELOAD_OK(s.loader.request_reload(std::move(*v3.module)));
    s.world.Tick(kDt);
    EXPECT_EQ(hot::active_generation, 3u);
    if (is_files_backend(*backend))
    {
        const std::string slots = slots_text(*backend);
        // The slot line has the number after the name. The signature line has the signature.
        const size_t first = slots.find("Medic::rest 1");
        ASSERT_NE(first, std::string::npos) << slots;
        EXPECT_EQ(slots.find("Medic::rest 1", first + 1), std::string::npos) << slots;
        EXPECT_NE(slots.find("sig Medic::rest void(Actor*, int32_t)"), std::string::npos) << slots;
    }
}

// A host call of a removed function through the old thunk throws ScriptError with the name of the
// function. The call does not run the slot: a removed function can return a class by value, and the
// caller then owns a result buffer. The call is a normal script error: the outermost call reports it
// to the error handler and gives a default result (an empty string, 0, a null handle). A call inside
// another call lets the error pass to the caller. The log line comes once for each function.
TEST_P(BackendTests, CallOfARemovedFunctionThrows)
{
    hot::LoadResult v1 = backend->load(source("defunct_v1.hot"), 1);
    ASSERT_TRUE(v1) << v1.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Oracle"));
    s.commit_at_frame_boundary();

    auto label = reinterpret_cast<std::string (*)(Actor*)>(s.loader.thunk("Oracle::Label"));
    auto score = reinterpret_cast<int (*)(Actor*)>(s.loader.thunk("Oracle::Score"));
    auto friend_of = reinterpret_cast<std::shared_ptr<void> (*)(Actor*)>(s.loader.thunk("Oracle::Friend"));
    ASSERT_NE(label, nullptr);
    ASSERT_NE(score, nullptr);
    ASSERT_NE(friend_of, nullptr);
    EXPECT_EQ(label(s.actor), "gen1 label that does not fit in a small buffer");
    EXPECT_EQ(score(s.actor), 7);
    EXPECT_NE(friend_of(s.actor), nullptr);

    hot::LoadResult v2 = backend->load(source("defunct_v2.hot"), 2); // after v1: it reads the slots of v1
    ASSERT_TRUE(v2) << v2.error;
    ASSERT_RELOAD_OK(s.loader.request_reload(std::move(*v2.module)));
    s.world.Tick(kDt); // the frame boundary commits: the generation is live
    ASSERT_EQ(hot::active_generation, 2u);

    const uint64_t calls0 = hot::removed_call_count();
    std::vector<std::string> reports;
    hot::set_script_error_handler([&reports](const hot::ScriptErrorReport& r) { reports.push_back(r.message); });
    testing::internal::CaptureStderr();
    // The outermost call reports the error and gives a default result.
    EXPECT_EQ(label(s.actor), "");
    EXPECT_EQ(label(s.actor), "");
    EXPECT_EQ(score(s.actor), 0);
    EXPECT_EQ(friend_of(s.actor), nullptr);
    hot::set_script_error_handler(nullptr);
    ASSERT_EQ(reports.size(), 4u);
    EXPECT_NE(reports[0].find("call of removed script function Oracle::Label"), std::string::npos) << reports[0];
    EXPECT_NE(reports[2].find("call of removed script function Oracle::Score"), std::string::npos) << reports[2];
    EXPECT_NE(reports[3].find("call of removed script function Oracle::Friend"), std::string::npos) << reports[3];
    // A call inside another call (the depth is 1 more): the error passes to the caller.
    uint32_t& depth = hot::detail::call_depth();
    ++depth;
    auto thrown = [&](auto&& call) {
        std::string what;
        try
        {
            call();
        }
        catch (const hot::ScriptError& e)
        {
            what = e.what();
        }
        return what;
    };
    const std::string w_label = thrown([&] { label(s.actor); });
    const std::string w_score = thrown([&] { score(s.actor); });
    const std::string w_friend = thrown([&] { friend_of(s.actor); });
    --depth;
    EXPECT_NE(w_label.find("call of removed script function Oracle::Label"), std::string::npos) << w_label;
    EXPECT_NE(w_score.find("call of removed script function Oracle::Score"), std::string::npos) << w_score;
    EXPECT_NE(w_friend.find("call of removed script function Oracle::Friend"), std::string::npos) << w_friend;
    const std::string log = testing::internal::GetCapturedStderr();
    // One log line for each function, although each function was called more than once.
    for (const char* fn : {"Oracle::Label", "Oracle::Score", "Oracle::Friend"})
    {
        const std::string line = std::string("call of removed script function ") + fn + " (";
        const size_t at = log.find(line);
        // A test that ran before in the process can have logged the line already.
        EXPECT_TRUE(at == std::string::npos || log.find(line, at + 1) == std::string::npos) << log;
    }
    EXPECT_EQ(hot::removed_call_count(), calls0 + 7);
    EXPECT_EQ(hot::active_generation, 2u);
    s.world.Tick(kDt); // the object still works
}

// A script with a compile error is a failed reload, not a full reload.
TEST_P(BackendTests, CompileErrorIsAFailedReload)
{
    // Build the bad script first, as generation 1. The slots file has no state of player_v1 yet.
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

    const bool files = is_files_backend(*backend);
    std::string before;
    if (files)
    {
        before = slots_text(*backend);
        EXPECT_FALSE(before.empty());
    }
    hot::LoadResult fresh = backend->load(source("fnremoved_v2.hot"), 1, hot::LoadMode::Fresh);
    ASSERT_TRUE(fresh) << fresh.error;
    EXPECT_FALSE(fresh.module->origin.empty());
    if (files)
        EXPECT_EQ(slots_text(*backend), before); // the build did not write
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
        shell = static_cast<hot::ShellBase*>(inst.shell);
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

    if (is_files_backend(*backend))
    {
        // The slots file has the slots of the fresh generation only: S got its own h().
        const std::string slots = slots_text(*backend);
        EXPECT_NE(slots.find("S::h"), std::string::npos) << slots;
    }

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

// A change of the script parent needs a full reload. The fresh slots file starts again.
TEST_P(BackendTests, FullReloadSequenceAfterAScriptParentChange)
{
    hot::LoadResult v1 = backend->load(source("fnremoved_v1.hot"), 1);
    ASSERT_TRUE(v1) << v1.error;
    FullReloadHost host;
    ASSERT_RELOAD_OK(host.loader.load(std::move(*v1.module)));
    ASSERT_NO_FATAL_FAILURE(host.create("Medic"));
    *host.field<int>("hp") = 42;
    if (is_files_backend(*backend))
    {
        EXPECT_NE(slots_text(*backend).find("Medic::rest"), std::string::npos);
    }

    // A live reload removes rest(): its slot is defunct and its line stays in the slots file.
    hot::LoadResult removed = backend->load(source("fnremoved_v2.hot"), 2);
    ASSERT_TRUE(removed) << removed.error;
    ASSERT_RELOAD_OK(host.loader.request_reload(std::move(*removed.module)));
    ASSERT_RELOAD_OK(host.loader.commit_pending());
    ASSERT_EQ(hot::active_generation, 2u);
    EXPECT_EQ(*host.field<int>("hp"), 42);
    if (is_files_backend(*backend))
        EXPECT_NE(slots_text(*backend).find("Medic::rest 1"), std::string::npos) << slots_text(*backend);

    hot::LoadResult v2 = backend->load(source("medic_reparent.hot"), 3);
    ASSERT_TRUE(v2) << v2.error;
    const hot::ReloadResult req = host.loader.request_reload(std::move(*v2.module));
    ASSERT_EQ(req.status, hot::ReloadStatus::FullReloadRequired) << req.error;
    EXPECT_EQ(req.reason, hot::ReloadReason::ScriptParentChanged);

    const int saved_hp = *host.field<int>("hp");
    std::string error;
    EXPECT_FALSE(host.loader.reset(error)); // the object is still held
    EXPECT_EQ(error, "1 live objects remain: Medic x1");
    host.drop();
    ASSERT_TRUE(host.loader.reset(error)) << error;

    hot::LoadResult fresh = backend->load(source("medic_reparent.hot"), 1, hot::LoadMode::Fresh);
    ASSERT_TRUE(fresh) << fresh.error;
    ASSERT_RELOAD_OK(host.loader.load(std::move(*fresh.module)));
    ASSERT_NO_FATAL_FAILURE(host.create("Medic"));
    *host.field<int>("hp") = saved_hp;
    EXPECT_EQ(*host.field<int>("hp"), 42);
    EXPECT_EQ(hot::active_generation, 1u);
    if (is_files_backend(*backend))
    {
        const std::string slots = slots_text(*backend);
        EXPECT_NE(slots.find("Medic::rest"), std::string::npos) << slots;
        EXPECT_NE(slots.find("Medic::heal"), std::string::npos) << slots;
    }

    // A fresh load of a script without rest() drops the defunct entry of the old slots file.
    host.drop();
    ASSERT_TRUE(host.loader.reset(error)) << error;
    hot::LoadResult without = backend->load(source("fnremoved_v2.hot"), 1, hot::LoadMode::Fresh);
    ASSERT_TRUE(without) << without.error;
    ASSERT_RELOAD_OK(host.loader.load(std::move(*without.module)));
    if (is_files_backend(*backend))
    {
        const std::string slots = slots_text(*backend);
        EXPECT_EQ(slots.find("Medic::rest"), std::string::npos) << slots;
        EXPECT_NE(slots.find("Medic::heal"), std::string::npos) << slots;
    }
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
// so reset fails with "generation 1 is pinned (a closure or a task is alive)" until a script function clears the variable.
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
    EXPECT_EQ(error, "generation 1 is pinned (a closure or a task is alive)");
    ASSERT_NO_FATAL_FAILURE(host.create("Holder"));
    clear(host.actor); // the script function releases the closure
    host.drop();
    EXPECT_TRUE(host.loader.reset(error)) << error;
}

} // namespace
