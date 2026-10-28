// Experiments R3 (escaped closure) and R6 (unload of a retired generation).
//
// The scripts timer_v1 and timer_v2 define the class Beeper. The method arm
// stores an arrow lambda in a World timer. The tests use the backend that
// SPP_BACKEND selected (make_default_backend).
//
// Finding for R6: the first generation cannot unload while its class lives.
// The class record, the factory, the thunks and the vtable of the first object
// are in the module of generation 1. The loader keeps that module (code_refs).
// The unload tests use generation 2 of a chain 1 -> 2 -> 3.
//
// CMake defines SPP_SOURCE_DIR.
#include <gtest/gtest.h>

#include <fstream>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#include "backend.h"
#include "test_support.h"

namespace {

using script::GenerationInfo;
using script::GenState;
using test_support::kDt;
using test_support::Session;

script::ScriptSource source(const char* file)
{
    const std::string path = std::string(SPP_SOURCE_DIR) + "/scripts/" + file;
    std::ifstream f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    return {path, ss.str()};
}

const GenerationInfo* find_gen(const script::LoaderStats& st, uint32_t generation)
{
    for (const GenerationInfo& g : st.generations)
        if (g.generation == generation) return &g;
    return nullptr;
}

int count_lines(const std::string& text, const std::string& needle)
{
    int n = 0;
    std::istringstream in(text);
    for (std::string l; std::getline(in, l);)
        if (l.find(needle) != std::string::npos) ++n;
    return n;
}

// ------------------------------------------------------------ Closure (no backend)

class ClosureTests : public ::testing::Test {
protected:
    void SetUp() override { script::active_generation = 9001; }
    void TearDown() override { script::active_generation = 0; }
};

TEST_F(ClosureTests, PinsFollowTheCopiesAndMoves)
{
    EXPECT_EQ(script::pin_count(9001), 0u);
    {
        script::Closure<void()> a(9001, "a", [] {});
        EXPECT_EQ(script::pin_count(9001), 1u);
        script::Closure<void()> b = a;
        EXPECT_EQ(script::pin_count(9001), 2u);
        script::Closure<void()> c = std::move(b);
        EXPECT_EQ(script::pin_count(9001), 2u);
        a = c;
        EXPECT_EQ(script::pin_count(9001), 2u);
        std::function<void()> f = c;  // a std::function holds one more pin
        EXPECT_EQ(script::pin_count(9001), 3u);
    }
    EXPECT_EQ(script::pin_count(9001), 0u);
}

TEST_F(ClosureTests, GuardRunsTheBodyOnlyInTheLiveGeneration)
{
    int runs = 0;
    script::Closure<int(int)> c(9001, "c", [&runs](int x) { ++runs; return x + 1; });
    EXPECT_EQ(c(1), 2);
    EXPECT_EQ(runs, 1);
    const uint64_t stale0 = script::stale_call_count();
    script::active_generation = 9002;
    EXPECT_EQ(c(1), 0);  // R(): a default value
    EXPECT_EQ(runs, 1);
    EXPECT_EQ(script::stale_call_count(), stale0 + 1);
    script::active_generation = 9001;
    EXPECT_EQ(c(5), 6);
}

TEST_F(ClosureTests, ConvertsToStdFunctionAndToADelegate)
{
    static_assert(std::is_convertible_v<script::Closure<void(float)>, std::function<void(float)>>);
    float seen = 0;
    auto c = script::make_closure(9001, "m", [&seen](float x) { seen = x; });
    static_assert(std::is_same_v<decltype(c), script::Closure<void(float)>>);
    Delegate<void(float)> d = c;
    ASSERT_TRUE(d.IsBound());
    d(2.5f);
    EXPECT_EQ(seen, 2.5f);
    std::function<void(float)> f = c;
    f(4.0f);
    EXPECT_EQ(seen, 4.0f);
    EXPECT_EQ(script::pin_count(9001), 3u);  // c, d, f
}

TEST_F(ClosureTests, PinCountIsAtomic)
{
    // Not a proof, but a thread sanitizer or a lost update shows here.
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t)
        threads.emplace_back([] {
            for (int i = 0; i < 10000; ++i) {
                script::pin(9100);
                script::unpin(9100);
            }
        });
    for (auto& t : threads) t.join();
    EXPECT_EQ(script::pin_count(9100), 0u);
}

// ------------------------------------------------------------ R3 and R6 with a backend

class ReloadTests : public ::testing::Test {
public:
    void SetUp() override { backend = script::make_default_backend(); }

    script::Module build(const char* file, uint32_t generation)
    {
        script::LoadResult r = backend->load(source(file), generation);
        if (!r) throw std::runtime_error(r.error);
        return std::move(*r.module);
    }

    // Reload to the module and tick once: the commit runs at the frame boundary.
    void reload(Session& s, script::Module m)
    {
        std::string error;
        ASSERT_TRUE(s.loader.request_reload(std::move(m), error)) << error;
        s.world.Tick(kDt);
        ASSERT_FALSE(s.commit_failed);
    }

    // Calls Beeper::arm through the thunk. The timer fires after "delay" seconds.
    void arm(Session& s, float delay)
    {
        using ArmFn = void (*)(Actor*, World&, float);
        void* thunk = s.loader.thunk("Beeper::arm");
        ASSERT_NE(thunk, nullptr);
        reinterpret_cast<ArmFn>(thunk)(s.actor, s.world, delay);
    }

    int* int_field(Session& s, const char* name) { return script::field_ptr<int>(s.inst.shell, name); }

    std::unique_ptr<script::Backend> backend;
};

// R3: a closure of generation 1 is in the world timer. After the reload to
// generation 2 it fires. The guard logs a stale call. The body does not run.
TEST_F(ReloadTests, R3_EscapedClosureIsStaleAfterReload)
{
    script::Module m1 = build("timer_v1.script", 1);
    script::Module m2 = build("timer_v2.script", 2);
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(m1), "Beeper"));
    s.commit_at_frame_boundary();
    Actor* const addr = s.actor;

    ASSERT_NO_FATAL_FAILURE(arm(s, 1.0f));
    EXPECT_EQ(s.world.TimerCount(), 1u);
    EXPECT_EQ(script::pin_count(1), 1u);  // the closure in the timer

    ASSERT_NO_FATAL_FAILURE(reload(s, std::move(m2)));
    EXPECT_EQ(script::active_generation, 2u);
    EXPECT_EQ(s.world.TimerCount(), 1u);
    EXPECT_EQ(script::pin_count(1), 1u);
    {
        const script::LoaderStats st = s.loader.stats();
        ASSERT_NE(find_gen(st, 1), nullptr);
        EXPECT_EQ(find_gen(st, 1)->state, GenState::Retired);
        EXPECT_EQ(find_gen(st, 1)->pins, 1u);
    }

    const uint64_t stale0 = script::stale_call_count();
    testing::internal::CaptureStdout();
    testing::internal::CaptureStderr();
    s.world.Tick(1.0f);  // the timer fires here
    const std::string log = testing::internal::GetCapturedStderr();
    const std::string out = testing::internal::GetCapturedStdout();

    EXPECT_EQ(script::stale_call_count(), stale0 + 1);
    EXPECT_EQ(count_lines(log, "stale call ignored: Beeper::arm lambda"), 1) << log;
    EXPECT_EQ(count_lines(out, "[timer"), 0) << out;  // no body ran
    ASSERT_NE(int_field(s, "fired"), nullptr);
    EXPECT_EQ(*int_field(s, "fired"), 0);  // the state is untouched
    EXPECT_EQ(s.world.TimerCount(), 0u);
    EXPECT_EQ(script::pin_count(1), 0u);  // the timer dropped the closure
    EXPECT_EQ(s.world.Actors()[0], addr);
}

// Without a reload the closure runs its body.
TEST_F(ReloadTests, R3_ClosureOfTheLiveGenerationRuns)
{
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(build("timer_v1.script", 1), "Beeper"));
    ASSERT_NO_FATAL_FAILURE(arm(s, 0.5f));
    const uint64_t stale0 = script::stale_call_count();
    s.world.Tick(1.0f);
    EXPECT_EQ(*int_field(s, "fired"), 1);
    EXPECT_EQ(script::stale_call_count(), stale0);
    EXPECT_EQ(script::pin_count(1), 0u);
}

// The closure of generation 2 fires after generation 3 is live.
// A chain keeps the test free of the first-generation anchor.
struct Chain {
    explicit Chain(ReloadTests& t)
    {
        m1 = t.build("timer_v1.script", 1);
        m2 = t.build("timer_v2.script", 2);
        m3 = t.build("timer_v2.script", 3);  // the same text, a new generation
    }
    script::Module m1, m2, m3;
};

// R6: the closure fired and was dropped. One frame later generation 2 is unloaded.
// Later ticks are clean. The object keeps its address.
TEST_F(ReloadTests, R6_GenerationUnloadsAfterTheClosureIsDropped)
{
    Chain c(*this);
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(c.m1), "Beeper"));
    s.commit_at_frame_boundary();
    Actor* const addr = s.actor;

    ASSERT_NO_FATAL_FAILURE(reload(s, std::move(c.m2)));
    ASSERT_NO_FATAL_FAILURE(arm(s, 1.0f));  // the closure belongs to generation 2
    EXPECT_EQ(script::pin_count(2), 1u);
    ASSERT_NO_FATAL_FAILURE(reload(s, std::move(c.m3)));
    EXPECT_EQ(script::active_generation, 3u);
    s.world.Tick(kDt);
    {
        const script::LoaderStats st = s.loader.stats();
        ASSERT_NE(find_gen(st, 2), nullptr);
        EXPECT_EQ(find_gen(st, 2)->state, GenState::Retired);  // the pin holds it
        EXPECT_EQ(find_gen(st, 2)->pins, 1u);
    }

    testing::internal::CaptureStderr();
    s.world.Tick(1.0f);  // fires (stale), drops the closure, then the boundary unloads
    const std::string log = testing::internal::GetCapturedStderr();
    EXPECT_EQ(count_lines(log, "stale call ignored: Beeper::arm lambda"), 1) << log;
    s.world.Tick(kDt);  // one more frame

    const script::LoaderStats st = s.loader.stats();
    ASSERT_NE(find_gen(st, 2), nullptr);
    EXPECT_EQ(find_gen(st, 2)->state, GenState::Unloaded);
    EXPECT_EQ(find_gen(st, 2)->pins, 0u);
    EXPECT_EQ(find_gen(st, 3)->state, GenState::Active);
    EXPECT_EQ(st.active_generation, 3u);
    EXPECT_EQ(st.live_objects, 1u);

    // Later ticks are clean, the object is the same, and a new closure works.
    const int ticks0 = *int_field(s, "ticks");
    for (int i = 0; i < 5; ++i) s.world.Tick(kDt);
    EXPECT_EQ(*int_field(s, "ticks"), ticks0 + 5);
    EXPECT_EQ(s.world.Actors()[0], addr);
    ASSERT_NO_FATAL_FAILURE(arm(s, 0.5f));
    testing::internal::CaptureStdout();
    s.world.Tick(1.0f);
    const std::string out2 = testing::internal::GetCapturedStdout();
    EXPECT_EQ(count_lines(out2, "[timer v2]"), 1) << out2;
    EXPECT_EQ(*int_field(s, "fired"), 10);
}

// A closure that stays alive keeps the generation loaded (pin count one).
TEST_F(ReloadTests, R6_AliveClosureKeepsTheGenerationLoaded)
{
    Chain c(*this);
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(c.m1), "Beeper"));
    s.commit_at_frame_boundary();
    ASSERT_NO_FATAL_FAILURE(reload(s, std::move(c.m2)));
    ASSERT_NO_FATAL_FAILURE(arm(s, 1000.0f));  // does not fire in this test
    ASSERT_NO_FATAL_FAILURE(reload(s, std::move(c.m3)));
    for (int i = 0; i < 5; ++i) s.world.Tick(kDt);

    script::LoaderStats st = s.loader.stats();
    ASSERT_NE(find_gen(st, 2), nullptr);
    EXPECT_EQ(find_gen(st, 2)->state, GenState::Retired);
    EXPECT_EQ(find_gen(st, 2)->pins, 1u);

    // The timer fires and drops the closure; then the generation unloads.
    s.world.Tick(2000.0f);
    s.world.Tick(kDt);
    st = s.loader.stats();
    EXPECT_EQ(find_gen(st, 2)->state, GenState::Unloaded);
    EXPECT_EQ(find_gen(st, 2)->pins, 0u);
}

// ClearTimers drops the closure too, so the generation can unload.
TEST_F(ReloadTests, R6_ClearTimersReleasesThePin)
{
    Chain c(*this);
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(c.m1), "Beeper"));
    s.commit_at_frame_boundary();
    ASSERT_NO_FATAL_FAILURE(reload(s, std::move(c.m2)));
    ASSERT_NO_FATAL_FAILURE(arm(s, 1000.0f));
    ASSERT_NO_FATAL_FAILURE(reload(s, std::move(c.m3)));
    s.world.ClearTimers();
    EXPECT_EQ(script::pin_count(2), 0u);
    s.world.Tick(kDt);
    s.world.Tick(kDt);
    EXPECT_EQ(find_gen(s.loader.stats(), 2)->state, GenState::Unloaded);
}

// Finding: generation 1 holds the record, the factory, the thunks and the vtable
// of the first object. It stays retired after the closure is dropped.
TEST_F(ReloadTests, R6_FirstGenerationStaysLoadedWhileItsClassLives)
{
    Session s;
    script::Module m2 = build("timer_v2.script", 2);
    ASSERT_NO_FATAL_FAILURE(s.start(build("timer_v1.script", 1), "Beeper"));
    s.commit_at_frame_boundary();
    ASSERT_NO_FATAL_FAILURE(arm(s, 1.0f));
    ASSERT_NO_FATAL_FAILURE(reload(s, std::move(m2)));
    testing::internal::CaptureStderr();
    s.world.Tick(1.0f);
    testing::internal::GetCapturedStderr();
    for (int i = 0; i < 3; ++i) s.world.Tick(kDt);

    const script::LoaderStats st = s.loader.stats();
    const GenerationInfo* g1 = find_gen(st, 1);
    ASSERT_NE(g1, nullptr);
    EXPECT_EQ(g1->state, GenState::Retired);
    EXPECT_EQ(g1->pins, 0u);
    EXPECT_GT(g1->code_refs, 0u);
    // The object still ticks, and the virtual call goes through the vtable of generation 1.
    const int ticks0 = *int_field(s, "ticks");
    s.world.Tick(kDt);
    EXPECT_EQ(*int_field(s, "ticks"), ticks0 + 1);
}

// The stats report the active generation, all generations and the live objects.
TEST_F(ReloadTests, StatsListGenerationsInOrder)
{
    Session s;
    script::Module m2 = build("timer_v2.script", 2);
    ASSERT_NO_FATAL_FAILURE(s.start(build("timer_v1.script", 1), "Beeper"));
    s.commit_at_frame_boundary();
    script::LoaderStats st = s.loader.stats();
    ASSERT_EQ(st.generations.size(), 1u);
    EXPECT_EQ(st.generations[0].state, GenState::Active);
    EXPECT_EQ(st.active_generation, 1u);
    EXPECT_EQ(st.live_objects, 1u);
    ASSERT_NO_FATAL_FAILURE(reload(s, std::move(m2)));
    st = s.loader.stats();
    ASSERT_EQ(st.generations.size(), 2u);
    EXPECT_EQ(st.generations[0].state, GenState::Retired);
    EXPECT_EQ(st.generations[1].state, GenState::Active);
    EXPECT_EQ(st.active_generation, 2u);
}

}  // namespace
