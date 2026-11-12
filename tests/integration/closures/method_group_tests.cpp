// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Method groups of a .hot script in native holders over a reload commit (doc/lambdas.md,
// "Method groups"). A bound group is a lambda box with the key part "m:<object>.<Method>". The
// commit gives the box the new body, or detaches it. An unbound group "C.M" is a hot::MethodRef: it
// calls through the slot table. The holders of tests/engine/Signal.h have the overload forms of
// the engine templates.
//
// The script text comes from script(v). The tests run with the clang files backend and with the
// JIT backend. A backend that is not in the build is skipped.
//
// CMake defines HOT_SOURCE_DIR.
#include "hot/backend.h"
#include "hot/runtime.h"
#include "support/test_support.h"

#include "Signal.h"

#include <gtest/gtest.h>

#include <functional>
#include <memory>
#include <string>

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
                    "using \"Signal.h\";\n"
                    "class Helper {\n"
                    "    int k = 5;\n";
    s += two ? "    int Add(int x) { return x + k * 100; }\n" : "    int Add(int x) { return x + k; }\n";
    s += "}\n"
         "class Mg : Actor {\n"
         "    int got = 0;\n";
    s += two ? "    int Twice(int x) { return x * 2 + " + V + "00; }\n" : "    int Twice(int x) { return x * 2; }\n";
    if (!two)
        s += "    int Gone(int x) { return x + 1; }\n";
    s += two ? "    long Sig(long x) { return x + 2; }\n" : "    int Sig(int x) { return x + 1; }\n";
    s += "    void OnHit(int n) { got = got + n * " + V + "; }\n";
    s += "    void OnEvt(int t, float v) { got = got + t * " + V + "; }\n";
    s += "    void Keep(ref std.function<int(int)> dst, std.function<int(int)> f) { dst = f; }\n"
         "    void bound(ref std.function<int(int)> dst) { Keep(dst, Twice); }\n"
         "    void selfref(ref std.function<int(int)> dst) { Keep(dst, this.Twice); }\n";
    s += two ? "    void gone(ref std.function<int(int)> dst) { }\n"
             : "    void gone(ref std.function<int(int)> dst) { Keep(dst, Gone); }\n";
    s += "    void sig(ref std.function<int(int)> dst) { Keep(dst, Sig); }\n"
         "    void other(ref std.function<int(int)> dst) { var h = new $Helper(); Keep(dst, h.Add); }\n"
         "    void sigsub(ref Signal<int> sg) { sg.Subscribe(this, OnHit); sg.Subscribe(this, Mg.OnHit); }\n"
         "    void evt(ref EventHub hub) { hub.SubscribeToEvent(7, OnEvt); }\n"
         "    void read(ref int dst) { dst = got; }\n"
         "}\n";
    return s;
}

class MethodGroupScript : public ::testing::TestWithParam<hot::BackendKind>
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
        const std::string path = std::string(HOT_SOURCE_DIR) + "/tests/scripts/method_group.hot";
        hot::LoadResult r = backend->load({path, script(v), {}}, generation);
        if (!r)
            throw std::runtime_error(r.error);
        return std::move(*r.module);
    }

    void start(Session& s)
    {
        ASSERT_NO_FATAL_FAILURE(s.start(build(1, 1), "Mg"));
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

    // Calls the script method "name" through its thunk.
    template <class... A> void call(Session& s, const char* name, A&... a)
    {
        using Fn = void (*)(Actor*, A&...);
        void* thunk = s.loader.thunk(name);
        ASSERT_NE(thunk, nullptr) << name;
        reinterpret_cast<Fn>(thunk)(s.actor, a...);
    }

    int got(Session& s)
    {
        int v = -1;
        call(s, "Mg::read", v);
        return v;
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

// A bound group ("Twice" and "this.Twice") in a std::function keeps the new body.
TEST_P(MethodGroupScript, BoundGroupRunsTheNewMethodBodyAfterReload)
{
    Session s;
    ASSERT_NO_FATAL_FAILURE(start(s));
    IntFn bare, self;
    ASSERT_NO_FATAL_FAILURE(call(s, "Mg::bound", bare));
    ASSERT_NO_FATAL_FAILURE(call(s, "Mg::selfref", self));
    EXPECT_EQ(bare(3), 6);
    EXPECT_EQ(self(3), 6);
    reload(s, 2, 2);
    EXPECT_EQ(bare(3), 206);
    EXPECT_EQ(self(3), 206);
    reload(s, 3, 3);
    EXPECT_EQ(bare(3), 306);
}

// A bound group on another object captures the object. The field of the object keeps its value.
TEST_P(MethodGroupScript, BoundGroupOnAnotherObject)
{
    Session s;
    ASSERT_NO_FATAL_FAILURE(start(s));
    IntFn f;
    ASSERT_NO_FATAL_FAILURE(call(s, "Mg::other", f));
    EXPECT_EQ(f(1), 6);
    reload(s, 2, 2);
    EXPECT_EQ(f(1), 501);
}

// A removed method and a changed signature detach the box with a warning. A call is a stale call.
TEST_P(MethodGroupScript, RemovedMethodAndNewSignatureDetach)
{
    Session s;
    ASSERT_NO_FATAL_FAILURE(start(s));
    IntFn gone, sig, keep;
    ASSERT_NO_FATAL_FAILURE(call(s, "Mg::gone", gone));
    ASSERT_NO_FATAL_FAILURE(call(s, "Mg::sig", sig));
    ASSERT_NO_FATAL_FAILURE(call(s, "Mg::bound", keep));
    EXPECT_EQ(gone(1), 2);
    EXPECT_EQ(sig(1), 2);
    const std::string log = reload(s, 2, 2);
    EXPECT_NE(log.find("lambda method_group:Mg::gone/m:this.Gone: the lambda is removed; 1 stored closures are detached"),
        std::string::npos)
        << log;
    EXPECT_NE(log.find("lambda method_group:Mg::sig/m:this.Sig: the signature changed"), std::string::npos) << log;
    int r = -1;
    EXPECT_EQ(stale_calls(gone, 1, &r), 1);
    EXPECT_EQ(r, 0);
    EXPECT_EQ(stale_calls(sig, 1, &r), 1);
    EXPECT_EQ(r, 0);
    EXPECT_EQ(stale_calls(keep, 1, &r), 0);
    EXPECT_EQ(r, 202);
}

// The engine forms: Signal::Subscribe(receiver, handler) with a bound group and with an unbound
// group "Mg.OnHit", and SubscribeToEvent(type, handler) with a bound group.
TEST_P(MethodGroupScript, EngineHoldersRunTheNewMethodBodyAfterReload)
{
    Session s;
    ASSERT_NO_FATAL_FAILURE(start(s));
    Signal<int> sg;
    EventHub hub;
    ASSERT_NO_FATAL_FAILURE(call(s, "Mg::sigsub", sg));
    ASSERT_NO_FATAL_FAILURE(call(s, "Mg::evt", hub));
    ASSERT_EQ(sg.Count(), 2u);
    ASSERT_EQ(hub.Count(), 1u);
    sg(1);
    EXPECT_EQ(got(s), 2);
    hub.SendEvent(7, 0.5f);
    EXPECT_EQ(got(s), 9);
    reload(s, 2, 2);
    sg(1);
    EXPECT_EQ(got(s), 13);
    hub.SendEvent(7, 0.5f);
    EXPECT_EQ(got(s), 27);
    sg.Clear();
    hub.Clear();
}

const char* kind_name(const ::testing::TestParamInfo<hot::BackendKind>& i)
{
    return i.param == hot::BackendKind::ClangFiles ? "Files" : "Jit";
}

INSTANTIATE_TEST_SUITE_P(
    Backends, MethodGroupScript, ::testing::Values(hot::BackendKind::ClangFiles, hot::BackendKind::Jit), kind_name);

} // namespace
