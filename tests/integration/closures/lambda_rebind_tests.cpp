// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Stored .hot lambdas over a reload commit. The commit gives each box the code of the new
// generation and moves its captures by name and type. A box that cannot move detaches.
//
// The script text comes from script(v). Version 1 has the first form of each method. A version
// v >= 2 has the second form, and the bodies use the constant v. The tests run with the clang
// files backend and with the JIT backend. A backend that is not in the build is skipped.
//
// CMake defines HOT_SOURCE_DIR.
#include "hot/backend.h"
#include "hot/runtime.h"
#include "hot/runtime/lambda.h"
#include "support/reload_assert.h"
#include "support/test_support.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{

using test_support::kDt;
using test_support::Session;
using IntFn = std::function<int(int)>;

std::string script(int v)
{
    const std::string V = std::to_string(v);
    const bool two = v >= 2;
    std::string s = "using \"Actor.h\";\n"
                    "class Rb : Actor {\n"
                    "    int base = 0;\n";
    // The new body runs. The value capture "k" keeps its value.
    s += two ? "    void body(ref std.function<int(int)> dst) { int k = 3; dst = (int x) => x + k * " + V + "00; }\n"
             : "    void body(ref std.function<int(int)> dst) { int k = 3; dst = (int x) => x + k; }\n";
    // A shared cell.
    s += two ? "    void cell(ref std.function<int(int)> inc) { int n = 0; inc = (int d) => { n += d * 10; return n; }; }\n"
             : "    void cell(ref std.function<int(int)> inc) { int n = 0; inc = (int d) => { n += d; return n; }; }\n";
    // A removed capture.
    s += two ? "    void removed(ref std.function<int(int)> dst) { int a = 1; dst = (int x) => x + a * 1000; }\n"
             : "    void removed(ref std.function<int(int)> dst) { int a = 1; int b = 2; dst = (int x) => x + a + b; }\n";
    // An added capture.
    s += two ? "    void added(ref std.function<int(int)> dst) { int a = 1; int b = 2; dst = (int x) => x + a + b; }\n"
             : "    void added(ref std.function<int(int)> dst) { int a = 1; dst = (int x) => x + a; }\n";
    // A capture with a new type.
    s += two ? "    void retype(ref std.function<int(int)> dst) { double a = 1.0; dst = (int x) => { if (a > 0.5) { return x + 1; } return x; }; }\n"
             : "    void retype(ref std.function<int(int)> dst) { int a = 1; dst = (int x) => x + a; }\n";
    // A new signature.
    s += two ? "    void sig(ref std.function<int(int)> dst) { dst = (long x) => x + 1; }\n"
             : "    void sig(ref std.function<int(int)> dst) { dst = (int x) => x + 1; }\n";
    // A removed lambda.
    s += two ? "    void gone(ref std.function<int(int)> dst) { }\n"
             : "    void gone(ref std.function<int(int)> dst) { dst = (int x) => x + 1; }\n";
    // Named lambdas in a new order.
    s += two ? "    void named(ref std.function<int(int)> a, ref std.function<int(int)> b) {\n"
               "        b = [name(\"q\")] (int x) => x + 20;\n"
               "        a = [name(\"p\")] (int x) => x + 10;\n"
               "    }\n"
             : "    void named(ref std.function<int(int)> a, ref std.function<int(int)> b) {\n"
               "        a = [name(\"p\")] (int x) => x + 1;\n"
               "        b = [name(\"q\")] (int x) => x + 2;\n"
               "    }\n";
    // Lambdas in variables in a new order.
    s += two ? "    void vars(ref std.function<int(int)> a, ref std.function<int(int)> b) {\n"
               "        var g = (int x) => x + 20;\n"
               "        var f = (int x) => x + 10;\n"
               "        a = f;\n"
               "        b = g;\n"
               "    }\n"
             : "    void vars(ref std.function<int(int)> a, ref std.function<int(int)> b) {\n"
               "        var f = (int x) => x + 1;\n"
               "        var g = (int x) => x + 2;\n"
               "        a = f;\n"
               "        b = g;\n"
               "    }\n";
    // A rename with an alias.
    s += two ? "    void alias(ref std.function<int(int)> dst) { dst = [name(\"fresh\"), alias(\"old\")] (int x) => x + 10; }\n"
             : "    void alias(ref std.function<int(int)> dst) { dst = [name(\"old\")] (int x) => x + 1; }\n";
    // A nested lambda: the outer box holds the closure "add".
    s += two ? "    void nested(ref std.function<int(int)> dst) { int a = 1; var add = (int x) => x + a * 10; dst = (int y) => add(add(y)) + 1000; }\n"
             : "    void nested(ref std.function<int(int)> dst) { int a = 1; var add = (int x) => x + a; dst = (int y) => add(add(y)); }\n";
    // A native delegate holds a lambda that uses "this".
    s += "    void bind(ref Delegate<int(int)> d) { d = (int x) => x + this.base * " + V + "; }\n";
    s += "}\n";
    return s;
}

class LambdaRebindScript : public ::testing::TestWithParam<hot::BackendKind>
{
public:
    void SetUp() override
    {
        if (!hot::backend_available(GetParam()))
            GTEST_SKIP() << "the backend is not in this build";
        hot::DefaultBackendOptions o;
        o.kind = GetParam();
        o.include_dirs = {std::string(HOT_SOURCE_DIR) + "/tests/engine"};
        backend = hot::make_default_backend(o);
    }

    hot::Module build(int v, uint32_t generation)
    {
        const std::string path = std::string(HOT_SOURCE_DIR) + "/tests/scripts/lambda_rebind.hot";
        hot::LoadResult r = backend->load({path, script(v), {}}, generation);
        if (!r)
            throw std::runtime_error(r.error);
        return std::move(*r.module);
    }

    void start(Session& s)
    {
        ASSERT_NO_FATAL_FAILURE(s.start(build(1, 1), "Rb"));
        s.commit_at_frame_boundary();
    }

    // Reloads to version v and ticks once: the commit runs at the frame boundary. Returns the log.
    std::string reload(Session& s, int v, uint32_t generation)
    {
        hot::Module m = build(v, generation);
        testing::internal::CaptureStderr();
        const hot::ReloadResult r = s.loader.request_reload(std::move(m));
        s.world.Tick(kDt);
        const std::string log = testing::internal::GetCapturedStderr();
        EXPECT_TRUE(r) << r.error;
        EXPECT_FALSE(s.commit_failed);
        EXPECT_EQ(hot::active_generation, generation);
        return log;
    }

    template <class... A> void call(Session& s, const char* name, A&... a)
    {
        using Fn = void (*)(Actor*, A&...);
        void* thunk = s.loader.thunk(name);
        ASSERT_NE(thunk, nullptr) << name;
        reinterpret_cast<Fn>(thunk)(s.actor, a...);
    }

    // Calls f and returns the result and the count of stale calls.
    static int stale_calls(const IntFn& f, int x, int* result)
    {
        const uint64_t stale0 = hot::stale_call_count();
        testing::internal::CaptureStderr();
        *result = f(x);
        testing::internal::GetCapturedStderr();
        return static_cast<int>(hot::stale_call_count() - stale0);
    }

    std::unique_ptr<hot::Backend> backend;
};

TEST_P(LambdaRebindScript, StoredLambdaRunsNewBodyAfterReload)
{
    Session s;
    ASSERT_NO_FATAL_FAILURE(start(s));
    IntFn f;
    ASSERT_NO_FATAL_FAILURE(call(s, "Rb::body", f));
    EXPECT_EQ(f(1), 4);
    reload(s, 2, 2);
    EXPECT_EQ(f(1), 601); // the new body, the capture k = 3 kept its value
    reload(s, 3, 3);
    EXPECT_EQ(f(1), 901);
}

TEST_P(LambdaRebindScript, SharedCellAndStdFunctionCopyKeepTheirState)
{
    Session s;
    ASSERT_NO_FATAL_FAILURE(start(s));
    IntFn inc;
    ASSERT_NO_FATAL_FAILURE(call(s, "Rb::cell", inc));
    IntFn copy = inc; // a copy of the closure: the same box
    EXPECT_EQ(inc(1), 1);
    EXPECT_EQ(copy(1), 2);
    reload(s, 2, 2);
    EXPECT_EQ(inc(1), 12); // the cell kept 2, the new body adds 10
    EXPECT_EQ(copy(1), 22);
}

TEST_P(LambdaRebindScript, RemovedCaptureIsDestroyedAndTheBoxMoves)
{
    Session s;
    ASSERT_NO_FATAL_FAILURE(start(s));
    IntFn f;
    ASSERT_NO_FATAL_FAILURE(call(s, "Rb::removed", f));
    EXPECT_EQ(f(0), 3);
    reload(s, 2, 2);
    EXPECT_EQ(f(0), 1000);
}

// A host can run register_fn on a scratch registry to list the classes before it gives the module
// to the loader. The module must still give its lambdas to the loader registry.
TEST_P(LambdaRebindScript, ScratchRegistryBeforeTheLoadKeepsTheLambdas)
{
    Session s;
    hot::Module m1 = build(1, 1);
    {
        hot::Registry scratch;
        m1.register_fn(scratch);
    }
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(m1), "Rb"));
    s.commit_at_frame_boundary();
    IntFn f;
    ASSERT_NO_FATAL_FAILURE(call(s, "Rb::body", f));
    hot::Module m2 = build(2, 2);
    {
        hot::Registry scratch;
        m2.register_fn(scratch);
    }
    testing::internal::CaptureStderr();
    const hot::ReloadResult r = s.loader.request_reload(std::move(m2));
    s.world.Tick(kDt);
    const std::string log = testing::internal::GetCapturedStderr();
    EXPECT_TRUE(r) << r.error;
    EXPECT_EQ(log.find("detached"), std::string::npos) << log;
    EXPECT_EQ(f(1), 601);
}

// The body of a lambda is a generic C++ lambda. A call of a native member template on "this"
// needs the "template" keyword there.
TEST_P(LambdaRebindScript, NativeMemberTemplateCallInALambdaBody)
{
    const std::string text =
        "using \"TemplateHost.h\";\n"
        "class Th : TemplateHost {\n"
        "    void make(ref std.function<int(int)> a, ref std.function<int(int)> b) {\n"
        "        a = (int x) => { return x + Size<double>(); };\n"
        "        b = (int x) => { return x + this.Size<char>(); };\n"
        "    }\n"
        "}\n";
    hot::LoadResult r = backend->load({std::string(HOT_SOURCE_DIR) + "/tests/scripts/lambda_template.hot", text, {}}, 1);
    ASSERT_TRUE(r) << r.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*r.module), "Th"));
    IntFn a, b;
    ASSERT_NO_FATAL_FAILURE(call(s, "Th::make", a, b));
    EXPECT_EQ(a(1), 9);
    EXPECT_EQ(b(1), 2);
}

// The detach cases: an added capture, a new capture type, a new signature, a removed lambda. A
// call of a detached closure is a stale call. Each key logs one warning.
TEST_P(LambdaRebindScript, IncompatibleChangesDetach)
{
    Session s;
    ASSERT_NO_FATAL_FAILURE(start(s));
    IntFn added, retype, sig, gone, body;
    ASSERT_NO_FATAL_FAILURE(call(s, "Rb::added", added));
    ASSERT_NO_FATAL_FAILURE(call(s, "Rb::retype", retype));
    ASSERT_NO_FATAL_FAILURE(call(s, "Rb::sig", sig));
    ASSERT_NO_FATAL_FAILURE(call(s, "Rb::gone", gone));
    ASSERT_NO_FATAL_FAILURE(call(s, "Rb::body", body));
    EXPECT_EQ(added(0), 1);
    const std::string log = reload(s, 2, 2);
    EXPECT_NE(log.find("lambda lambda_rebind:Rb::added/v:dst: capture b is added; 1 stored closures are detached"),
        std::string::npos)
        << log;
    EXPECT_NE(log.find("capture a changed its type"), std::string::npos) << log;
    EXPECT_NE(log.find("the signature changed"), std::string::npos) << log;
    EXPECT_NE(log.find("the lambda is removed"), std::string::npos) << log;
    int r = -1;
    for (IntFn* f : {&added, &retype, &sig, &gone})
    {
        EXPECT_EQ(stale_calls(*f, 0, &r), 1);
        EXPECT_EQ(r, 0);
    }
    EXPECT_EQ(stale_calls(body, 0, &r), 0);
    EXPECT_EQ(r, 600);
}

TEST_P(LambdaRebindScript, NamedAndVariableLambdasSurviveAReorder)
{
    Session s;
    ASSERT_NO_FATAL_FAILURE(start(s));
    IntFn na, nb, va, vb;
    ASSERT_NO_FATAL_FAILURE(call(s, "Rb::named", na, nb));
    ASSERT_NO_FATAL_FAILURE(call(s, "Rb::vars", va, vb));
    EXPECT_EQ(na(0) * 10 + nb(0), 12);
    EXPECT_EQ(va(0) * 10 + vb(0), 12);
    reload(s, 2, 2);
    EXPECT_EQ(na(0), 10);
    EXPECT_EQ(nb(0), 20);
    EXPECT_EQ(va(0), 10);
    EXPECT_EQ(vb(0), 20);
}

TEST_P(LambdaRebindScript, AliasKeyFindsTheRenamedLambda)
{
    Session s;
    ASSERT_NO_FATAL_FAILURE(start(s));
    IntFn f;
    ASSERT_NO_FATAL_FAILURE(call(s, "Rb::alias", f));
    EXPECT_EQ(f(0), 1);
    reload(s, 2, 2);
    EXPECT_EQ(f(0), 10);
    EXPECT_NE(s.loader.find_lambda("lambda_rebind:Rb::alias/n:old"), nullptr); // the alias key finds the record
    reload(s, 3, 3); // the box has the new key now
    EXPECT_EQ(f(0), 10);
}

TEST_P(LambdaRebindScript, NestedLambdaRunsTheNewBodies)
{
    Session s;
    ASSERT_NO_FATAL_FAILURE(start(s));
    IntFn f;
    ASSERT_NO_FATAL_FAILURE(call(s, "Rb::nested", f));
    EXPECT_EQ(f(0), 2);
    reload(s, 2, 2);
    EXPECT_EQ(f(0), 1020);
}

// A native delegate holds a lambda that uses "this" over 3 reloads. The shell pointer stays
// valid: the object keeps its shell, and the commit gives it a new state block.
TEST_P(LambdaRebindScript, NativeDelegateKeepsWorkingOverThreeReloads)
{
    Session s;
    ASSERT_NO_FATAL_FAILURE(start(s));
    *hot::field_ptr<int>(s.inst.shell, "base") = 5;
    Delegate<int(int)> d;
    ASSERT_NO_FATAL_FAILURE(call(s, "Rb::bind", d));
    EXPECT_EQ(d(1), 6);
    for (int v = 2; v <= 4; ++v)
    {
        reload(s, v, static_cast<uint32_t>(v));
        EXPECT_EQ(d(1), 1 + 5 * v) << v;
    }
    // Only the generation of the glue code (1) and the active one stay loaded.
    s.world.Tick(kDt);
    size_t loaded = 0;
    for (const hot::GenerationInfo& g : s.loader.stats().generations)
        loaded += g.state != hot::GenState::Unloaded;
    EXPECT_LE(loaded, 2u) << s.loader.stats_text();
    d = Delegate<int(int)>();
    s.world.Tick(kDt);
    s.world.Tick(kDt);
    loaded = 0;
    for (const hot::GenerationInfo& g : s.loader.stats().generations)
        loaded += g.state != hot::GenState::Unloaded;
    EXPECT_EQ(loaded, 1u) << s.loader.stats_text();
}

// Worker threads copy, call and release closures while the main thread commits reloads (the
// EntryCounters mode). A worker drops the last copy of some closures during a commit.
TEST_P(LambdaRebindScript, WorkersCopyAndReleaseWhileReloadsCommit)
{
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(build(1, 1), "Rb"));
    s.loader.set_sync_mode(hot::SyncMode::EntryCounters);
    std::vector<hot::Module> mods;
    for (int v = 2; v <= 4; ++v)
        mods.push_back(build(v, static_cast<uint32_t>(v)));

    std::mutex lock;
    std::deque<IntFn> queue;
    std::atomic<bool> stop{false};
    std::atomic<int> calls{0};
    std::vector<std::thread> workers;
    for (int w = 0; w < 4; ++w)
        workers.emplace_back(
            [&]
            {
                while (!stop.load())
                {
                    IntFn f;
                    {
                        std::lock_guard<std::mutex> g(lock);
                        if (queue.empty())
                            continue;
                        f = std::move(queue.front());
                        queue.pop_front();
                    }
                    for (int i = 0; i < 4; ++i)
                    {
                        IntFn copy = f;
                        if (copy(1) > 0)
                            calls.fetch_add(1);
                    }
                }
            });
    const auto feed = [&]
    {
        for (int i = 0; i < 16; ++i)
        {
            IntFn f;
            call(s, "Rb::cell", f);
            std::lock_guard<std::mutex> g(lock);
            queue.push_back(std::move(f));
        }
    };
    testing::internal::CaptureStderr();
    for (int v = 2; v <= 4; ++v)
    {
        feed();
        ASSERT_RELOAD_OK(s.loader.request_reload(std::move(mods[v - 2])));
        feed();
        const hot::ReloadResult r = s.loader.commit_pending();
        EXPECT_TRUE(r) << r.error;
        s.loader.frame_boundary();
    }
    feed();
    // Wait for a first call: a slow start of a thread must not end the test with no call.
    for (int i = 0; i < 10000 && calls.load() == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    stop = true;
    for (std::thread& t : workers)
        t.join();
    queue.clear();
    testing::internal::GetCapturedStderr();
    EXPECT_GT(calls.load(), 0);
    s.loader.set_sync_mode(hot::SyncMode::StopTheWorld);
}

const char* kind_name(const ::testing::TestParamInfo<hot::BackendKind>& i)
{
    return i.param == hot::BackendKind::ClangFiles ? "Files" : "Jit";
}

INSTANTIATE_TEST_SUITE_P(
    Backends, LambdaRebindScript, ::testing::Values(hot::BackendKind::ClangFiles, hot::BackendKind::Jit), kind_name);

} // namespace
