// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The capture block of a stored lambda holds the last handle of a script object. A reload commit
// migrates the object. The old capture block dies when the new body drops the capture, removes
// the lambda or changes its signature. The destructor of the object must run after the commit
// switched the generation, and it must not free an object that the commit still uses. Run the
// tests under ASan to find a use after free.
#include "Util.h"
#include "hot/backend.h"
#include "hot/runtime.h"
#include "hot/runtime/threads.h"
#include "support/reload_assert.h"
#include "support/test_support.h"

#include <gtest/gtest.h>

#include <functional>
#include <memory>
#include <string>

namespace
{

using test_support::kDt;
using test_support::Session;
using IntFn = std::function<int(int)>;

// Version 1 has the first form. A version v >= 2 has the second form. Helper has a new field in
// version 2, so the commit migrates each Helper. The destructor of Helper creates and drops an
// object, to find a lock that the commit holds.
std::string script(int v)
{
    const bool two = v >= 2;
    std::string s = "using \"Actor.h\";\n"
                    "using \"Util.h\";\n"
                    "class Helper {\n"
                    "    int id = 0;\n";
    if (two)
        s += "    int extra = 5;\n";
    s += "    Helper(int i) { this.id = i; }\n"
         "    int tick() { return this.id; }\n"
         "    ~Helper() {\n"
         "        Util.Note(\"helper\");\n"
         "        if (this.id >= 0) { var t = new $Helper(-1); }\n"
         "    }\n"
         "}\n"
         "class Rb : Actor {\n";
    // The new body does not use the capture "h".
    s += two ? "    void dropped(ref std.function<int(int)> dst) { var h = new $Helper(7); dst = (int x) => x + 100; }\n"
             : "    void dropped(ref std.function<int(int)> dst) { var h = new $Helper(7); dst = (int x) => x + (h?.tick() ?? 0); }\n";
    // The new body has no lambda.
    s += two ? "    void gone(ref std.function<int(int)> dst) { var h = new $Helper(7); }\n"
             : "    void gone(ref std.function<int(int)> dst) { var h = new $Helper(7); dst = (int x) => x + (h?.tick() ?? 0); }\n";
    // The new lambda has a new signature.
    s += two ? "    void sig(ref std.function<int(int)> dst) { var h = new $Helper(7); dst = (long x) => x + (h?.tick() ?? 0); }\n"
             : "    void sig(ref std.function<int(int)> dst) { var h = new $Helper(7); dst = (int x) => x + (h?.tick() ?? 0); }\n";
    // The new body keeps the capture.
    s += two ? "    void kept(ref std.function<int(int)> dst) { var h = new $Helper(7); dst = (int x) => x + (h?.tick() ?? 0) * 10; }\n"
             : "    void kept(ref std.function<int(int)> dst) { var h = new $Helper(7); dst = (int x) => x + (h?.tick() ?? 0); }\n";
    s += "}\n";
    return s;
}

class LambdaCaptureDestroy : public ::testing::TestWithParam<hot::BackendKind>
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
        const std::string path = std::string(HOT_SOURCE_DIR) + "/tests/scripts/lambda_capture_destroy.hot";
        hot::LoadResult r = backend->load({path, script(v), {}}, generation);
        if (!r)
            throw std::runtime_error(r.error);
        return std::move(*r.module);
    }

    // Makes the closure of method "name", reloads to version 2 and returns the notes.
    std::string reload_with(Session& s, const char* name, IntFn& f)
    {
        s.start(build(1, 1), "Rb");
        s.commit_at_frame_boundary();
        void* thunk = s.loader.thunk(name);
        EXPECT_NE(thunk, nullptr) << name;
        reinterpret_cast<void (*)(Actor*, IntFn&)>(thunk)(s.actor, f);
        EXPECT_EQ(f(1), 8);
        Util::ClearNotes();
        hot::Module m = build(2, 2);
        const hot::ReloadResult r = s.loader.request_reload(std::move(m));
        s.world.Tick(kDt);
        EXPECT_TRUE(r) << r.error;
        EXPECT_FALSE(s.commit_failed);
        EXPECT_EQ(hot::active_generation, 2u);
        return Util::Notes();
    }

    // Each note has the generation and the link epoch of the finished commit.
    static void expect_after_switch(const std::string& notes)
    {
        const std::string tail = "@g2e" + std::to_string(hot::link_epoch) + ";";
        size_t n = 0;
        for (size_t p = notes.find("helper"); p != std::string::npos; p = notes.find("helper", p + 1))
        {
            ++n;
            EXPECT_EQ(notes.compare(p + 6, tail.size(), tail), 0) << notes;
        }
        EXPECT_GE(n, 2u) << notes; // the Helper of the box and the object that its destructor made
    }

    std::unique_ptr<hot::Backend> backend;
};

TEST_P(LambdaCaptureDestroy, DroppedCaptureDiesAfterTheSwitch)
{
    Session s;
    IntFn f;
    expect_after_switch(reload_with(s, "Rb::dropped", f));
    EXPECT_EQ(f(1), 101); // the box moved to the new body
}

TEST_P(LambdaCaptureDestroy, RemovedLambdaDiesAfterTheSwitch)
{
    Session s;
    IntFn f;
    expect_after_switch(reload_with(s, "Rb::gone", f));
    const uint64_t stale0 = hot::stale_call_count();
    testing::internal::CaptureStderr();
    EXPECT_EQ(f(1), 0);
    testing::internal::GetCapturedStderr();
    EXPECT_EQ(hot::stale_call_count() - stale0, 1u);
}

TEST_P(LambdaCaptureDestroy, ChangedSignatureDiesAfterTheSwitch)
{
    Session s;
    IntFn f;
    expect_after_switch(reload_with(s, "Rb::sig", f));
    const uint64_t stale0 = hot::stale_call_count();
    testing::internal::CaptureStderr();
    EXPECT_EQ(f(1), 0);
    testing::internal::GetCapturedStderr();
    EXPECT_EQ(hot::stale_call_count() - stale0, 1u);
}

TEST_P(LambdaCaptureDestroy, KeptCaptureMovesAndNothingDies)
{
    Session s;
    IntFn f;
    const std::string notes = reload_with(s, "Rb::kept", f);
    EXPECT_EQ(notes.find("helper"), std::string::npos) << notes;
    EXPECT_EQ(f(1), 71);
}

const char* kind_name(const ::testing::TestParamInfo<hot::BackendKind>& i)
{
    return i.param == hot::BackendKind::ClangFiles ? "Files" : "Jit";
}

INSTANTIATE_TEST_SUITE_P(
    Backends, LambdaCaptureDestroy, ::testing::Values(hot::BackendKind::ClangFiles, hot::BackendKind::Jit), kind_name);

} // namespace
