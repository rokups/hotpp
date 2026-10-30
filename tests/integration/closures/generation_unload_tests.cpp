// Escaped closures and the unload of a retired generation.
//
// The scripts timer_v1 and timer_v2 define the class Beeper. The method arm
// stores an arrow lambda in a World timer. The tests use the backend that
// SPP_BACKEND selected (make_default_backend).
//
// The class record, the factory, the thunks and the vtable of each object are in the core
// module (design 4.9, M8). So generation 1 unloads like a later generation. The first unload
// tests use generation 2 of a chain 1 -> 2 -> 3.
//
// CMake defines SPP_SOURCE_DIR.
#include "spp/backend.h"
#include "support/reload_assert.h"
#include "support/script_files.h"
#include "support/test_support.h"

#include <gtest/gtest.h>

#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace
{

using spp::GenerationInfo;
using spp::GenState;
using test_support::kDt;
using test_support::Session;
using test_support::source;

const GenerationInfo* find_gen(const spp::LoaderStats& st, uint32_t generation)
{
    for (const GenerationInfo& g : st.generations)
        if (g.generation == generation)
            return &g;
    return nullptr;
}

int count_lines(const std::string& text, const std::string& needle)
{
    int n = 0;
    std::istringstream in(text);
    for (std::string l; std::getline(in, l);)
        if (l.find(needle) != std::string::npos)
            ++n;
    return n;
}

class GenerationUnload : public ::testing::Test
{
public:
    void SetUp() override { backend = spp::make_default_backend(); }

    spp::Module build(const char* file, uint32_t generation)
    {
        spp::LoadResult r = backend->load(source(file), generation);
        if (!r)
            throw std::runtime_error(r.error);
        return std::move(*r.module);
    }

    // Reload to the module and tick once: the commit runs at the frame boundary.
    void reload(Session& s, spp::Module m)
    {
        std::string error;
        ASSERT_RELOAD_OK(s.loader.request_reload(std::move(m)));
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

    int* int_field(Session& s, const char* name) { return spp::field_ptr<int>(s.inst.shell, name); }

    std::unique_ptr<spp::Backend> backend;
};

// Escaped closure: a closure of generation 1 is in the world timer. After the reload to
// generation 2 it fires. The guard logs a stale call. The body does not run.
TEST_F(GenerationUnload, EscapedClosureIsStaleAfterReload)
{
    spp::Module m1 = build("timer_v1.spp", 1);
    spp::Module m2 = build("timer_v2.spp", 2);
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(m1), "Beeper"));
    s.commit_at_frame_boundary();
    Actor* const addr = s.actor;

    ASSERT_NO_FATAL_FAILURE(arm(s, 1.0f));
    EXPECT_EQ(s.world.TimerCount(), 1u);
    EXPECT_EQ(spp::pin_count(1), 1u); // the closure in the timer

    ASSERT_NO_FATAL_FAILURE(reload(s, std::move(m2)));
    EXPECT_EQ(spp::active_generation, 2u);
    EXPECT_EQ(s.world.TimerCount(), 1u);
    EXPECT_EQ(spp::pin_count(1), 1u);
    {
        const spp::LoaderStats st = s.loader.stats();
        ASSERT_NE(find_gen(st, 1), nullptr);
        EXPECT_EQ(find_gen(st, 1)->state, GenState::Retired);
        EXPECT_EQ(find_gen(st, 1)->pins, 1u);
    }

    const uint64_t stale0 = spp::stale_call_count();
    testing::internal::CaptureStdout();
    testing::internal::CaptureStderr();
    s.world.Tick(1.0f); // the timer fires here
    const std::string log = testing::internal::GetCapturedStderr();
    const std::string out = testing::internal::GetCapturedStdout();

    EXPECT_EQ(spp::stale_call_count(), stale0 + 1);
    EXPECT_EQ(count_lines(log, "stale call ignored: Beeper::arm lambda"), 1) << log;
    EXPECT_EQ(count_lines(out, "[timer"), 0) << out; // no body ran
    ASSERT_NE(int_field(s, "fired"), nullptr);
    EXPECT_EQ(*int_field(s, "fired"), 0); // the state is untouched
    EXPECT_EQ(s.world.TimerCount(), 0u);
    EXPECT_EQ(spp::pin_count(1), 0u); // the timer dropped the closure
    EXPECT_EQ(s.world.Actors()[0], addr);
}

// Without a reload the closure runs its body.
TEST_F(GenerationUnload, ClosureOfTheLiveGenerationRuns)
{
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(build("timer_v1.spp", 1), "Beeper"));
    ASSERT_NO_FATAL_FAILURE(arm(s, 0.5f));
    const uint64_t stale0 = spp::stale_call_count();
    s.world.Tick(1.0f);
    EXPECT_EQ(*int_field(s, "fired"), 1);
    EXPECT_EQ(spp::stale_call_count(), stale0);
    EXPECT_EQ(spp::pin_count(1), 0u);
}

// The closure of generation 2 fires after generation 3 is live.
// A chain keeps the test free of the first-generation anchor.
struct Chain
{
    explicit Chain(GenerationUnload& t)
    {
        m1 = t.build("timer_v1.spp", 1);
        m2 = t.build("timer_v2.spp", 2);
        m3 = t.build("timer_v2.spp", 3); // the same text, a new generation
    }
    spp::Module m1, m2, m3;
};

// Unload: the closure fired and was dropped. One frame later generation 2 is unloaded.
// Later ticks are clean. The object keeps its address.
TEST_F(GenerationUnload, GenerationUnloadsAfterTheClosureIsDropped)
{
    Chain c(*this);
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(c.m1), "Beeper"));
    s.commit_at_frame_boundary();
    Actor* const addr = s.actor;

    ASSERT_NO_FATAL_FAILURE(reload(s, std::move(c.m2)));
    ASSERT_NO_FATAL_FAILURE(arm(s, 1.0f)); // the closure belongs to generation 2
    EXPECT_EQ(spp::pin_count(2), 1u);
    ASSERT_NO_FATAL_FAILURE(reload(s, std::move(c.m3)));
    EXPECT_EQ(spp::active_generation, 3u);
    s.world.Tick(kDt);
    {
        const spp::LoaderStats st = s.loader.stats();
        ASSERT_NE(find_gen(st, 2), nullptr);
        EXPECT_EQ(find_gen(st, 2)->state, GenState::Retired); // the pin holds it
        EXPECT_EQ(find_gen(st, 2)->pins, 1u);
    }

    testing::internal::CaptureStderr();
    s.world.Tick(1.0f); // fires (stale), drops the closure, then the boundary unloads
    const std::string log = testing::internal::GetCapturedStderr();
    EXPECT_EQ(count_lines(log, "stale call ignored: Beeper::arm lambda"), 1) << log;
    s.world.Tick(kDt); // one more frame

    const spp::LoaderStats st = s.loader.stats();
    ASSERT_NE(find_gen(st, 2), nullptr);
    EXPECT_EQ(find_gen(st, 2)->state, GenState::Unloaded);
    EXPECT_EQ(find_gen(st, 2)->pins, 0u);
    EXPECT_EQ(find_gen(st, 3)->state, GenState::Active);
    EXPECT_EQ(st.active_generation, 3u);
    EXPECT_EQ(st.live_objects, 1u);

    // Later ticks are clean, the object is the same, and a new closure works.
    const int ticks0 = *int_field(s, "ticks");
    for (int i = 0; i < 5; ++i)
        s.world.Tick(kDt);
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
TEST_F(GenerationUnload, AliveClosureKeepsTheGenerationLoaded)
{
    Chain c(*this);
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(c.m1), "Beeper"));
    s.commit_at_frame_boundary();
    ASSERT_NO_FATAL_FAILURE(reload(s, std::move(c.m2)));
    ASSERT_NO_FATAL_FAILURE(arm(s, 1000.0f)); // does not fire in this test
    ASSERT_NO_FATAL_FAILURE(reload(s, std::move(c.m3)));
    for (int i = 0; i < 5; ++i)
        s.world.Tick(kDt);

    spp::LoaderStats st = s.loader.stats();
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
TEST_F(GenerationUnload, ClearTimersReleasesThePin)
{
    Chain c(*this);
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(c.m1), "Beeper"));
    s.commit_at_frame_boundary();
    ASSERT_NO_FATAL_FAILURE(reload(s, std::move(c.m2)));
    ASSERT_NO_FATAL_FAILURE(arm(s, 1000.0f));
    ASSERT_NO_FATAL_FAILURE(reload(s, std::move(c.m3)));
    s.world.ClearTimers();
    EXPECT_EQ(spp::pin_count(2), 0u);
    s.world.Tick(kDt);
    s.world.Tick(kDt);
    EXPECT_EQ(find_gen(s.loader.stats(), 2)->state, GenState::Unloaded);
}

// The core module holds the record, the factory, the thunks and the vtable of the object.
// So generation 1 unloads after the closure is dropped, while its object lives.
TEST_F(GenerationUnload, FirstGenerationUnloadsWhileItsObjectLives)
{
    Session s;
    spp::Module m2 = build("timer_v2.spp", 2);
    ASSERT_NO_FATAL_FAILURE(s.start(build("timer_v1.spp", 1), "Beeper"));
    s.commit_at_frame_boundary();
    ASSERT_NO_FATAL_FAILURE(arm(s, 1.0f));
    ASSERT_NO_FATAL_FAILURE(reload(s, std::move(m2)));
    testing::internal::CaptureStderr();
    s.world.Tick(1.0f);
    testing::internal::GetCapturedStderr();
    for (int i = 0; i < 3; ++i)
        s.world.Tick(kDt);

    const spp::LoaderStats st = s.loader.stats();
    const GenerationInfo* g1 = find_gen(st, 1);
    ASSERT_NE(g1, nullptr);
    EXPECT_EQ(g1->state, GenState::Unloaded);
    EXPECT_EQ(g1->pins, 0u);
    EXPECT_EQ(g1->code_refs, 0u);
    EXPECT_EQ(st.core_modules, 1u);
    EXPECT_EQ(st.live_objects, 1u);
    // The object still ticks: the virtual call goes through the vtable of the core module.
    const int ticks0 = *int_field(s, "ticks");
    s.world.Tick(kDt);
    EXPECT_EQ(*int_field(s, "ticks"), ticks0 + 1);
    // The thunk is in the core module too. The closure of generation 2 runs.
    ASSERT_NO_FATAL_FAILURE(arm(s, 0.5f));
    testing::internal::CaptureStdout();
    s.world.Tick(1.0f);
    const std::string out = testing::internal::GetCapturedStdout();
    EXPECT_EQ(count_lines(out, "[timer v2]"), 1) << out;
    // The engine factory of the core module makes a new object after the unload.
    const spp::ClassEntry* e = s.loader.find("Beeper");
    ASSERT_NE(e, nullptr);
    spp::Instance second = e->create();
    s.world.Add(static_cast<Actor*>(second.native));
    s.world.Tick(kDt);
    ASSERT_NE(spp::field_ptr<int>(second.shell, "ticks"), nullptr);
    EXPECT_EQ(*spp::field_ptr<int>(second.shell, "ticks"), 1);
}

// Many reloads with live objects of several generations: the count of loaded modules stays
// bounded. Each generation is a new compile, so the test uses a short chain. The fixture test
// ManyReloads (integration_reload_tests) runs hundreds of reloads.
TEST_F(GenerationUnload, LoadedModulesStayBoundedOverReloads)
{
    constexpr uint32_t kLast = 6;
    std::vector<spp::Module> mods;
    for (uint32_t g = 2; g <= kLast; ++g)
        mods.push_back(build("timer_v2.spp", g));
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(build("timer_v1.spp", 1), "Beeper"));
    s.commit_at_frame_boundary();
    std::vector<spp::Instance> objs{s.inst};
    testing::internal::CaptureStderr();
    for (uint32_t g = 2; g <= kLast; ++g)
    {
        ASSERT_NO_FATAL_FAILURE(reload(s, std::move(mods[g - 2])));
        ASSERT_NO_FATAL_FAILURE(arm(s, 0.0f)); // a closure of this generation; it fires next frame
        spp::Instance o = s.loader.find("Beeper")->create(); // an object of this generation
        s.world.Add(static_cast<Actor*>(o.native));
        objs.push_back(o);
        s.world.Tick(kDt);
        s.world.Tick(kDt);
        const spp::LoaderStats st = s.loader.stats();
        size_t loaded = st.core_modules;
        for (const GenerationInfo& gi : st.generations)
            loaded += gi.state != GenState::Unloaded;
        EXPECT_LE(loaded, 2u) << "generation " << g << "\n" << s.loader.stats_text();
    }
    testing::internal::GetCapturedStderr();
    const spp::LoaderStats st = s.loader.stats();
    for (uint32_t g = 1; g < kLast; ++g)
        EXPECT_EQ(find_gen(st, g)->state, GenState::Unloaded) << g;
    EXPECT_EQ(st.live_objects, objs.size());
    // All objects tick in the last generation.
    std::vector<int> before;
    for (const spp::Instance& o : objs)
        before.push_back(*spp::field_ptr<int>(o.shell, "ticks"));
    s.world.Tick(kDt);
    for (size_t i = 0; i < objs.size(); ++i)
        EXPECT_EQ(*spp::field_ptr<int>(objs[i].shell, "ticks"), before[i] + 1);
}

// The stats report the active generation, all generations and the live objects.
TEST_F(GenerationUnload, StatsListGenerationsInOrder)
{
    Session s;
    spp::Module m2 = build("timer_v2.spp", 2);
    ASSERT_NO_FATAL_FAILURE(s.start(build("timer_v1.spp", 1), "Beeper"));
    s.commit_at_frame_boundary();
    spp::LoaderStats st = s.loader.stats();
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

} // namespace
