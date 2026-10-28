// The tick loop on worker threads, a reload from a third thread.
//
// The world ticks 1000 Player actors on two worker threads (World::TickParallel).
// The main thread waits at the barrier and runs the frame boundary hook there.
// The hook commits a pending reload. A third thread, the "compile thread",
// requests the reload at a random moment while the workers run script code.
//
// The tests check that:
//   - the commit runs only at the barrier (no thread is in script code);
//   - no actor ticks with a state that is half migrated;
//   - every actor ends in the same generation with the same values;
//   - a rejected reload from the third thread changes nothing.
//
// The scripts are in this file and they do not print, so the output stays small.
// The tests use the backend that SPP_BACKEND selected (make_default_backend).
// CMake defines the macros GEN4_BASE_SO and GEN3_MISSING_SO.
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "spp/backend.h"
#include "support/test_support.h"

namespace {

using test_support::kDt;
using test_support::Session;

constexpr int kActors = 1000;
constexpr unsigned kWorkers = 2;
constexpr int kFrames = 200;
constexpr float kStartHealth = 10.0f;

const char* const kV1 = R"(
using "Actor.h";
using "Math.h";

class Player : Actor {
    float hp = 10.0;
    float speed = 5.0;

    override void Tick(float dt) {
        this.Move(Vec3(this.speed * dt, 0, 0));
        this.hp = Math.Min(this.hp + 1.0, 100000.0);
    }
}
)";

const char* const kV2 = R"(
using "Actor.h";
using "Math.h";

class Player : Actor {
    [alias("hp")] float health = 10.0;
    float speed = 5.0;
    float shield = 25.0;

    override void Tick(float dt) {
        this.Move(Vec3(2.0 * this.speed * dt, 0, 0));
    }
}
)";

spp::Backend& backend()
{
    static std::unique_ptr<spp::Backend> b = spp::make_default_backend();
    return *b;
}

// Gives a new Module of one script for each call. A build takes about a second.
// A library file is loaded again from its path.
// The loader of the last run is gone, so the old copy is closed.
// The new copy has fresh data. A backend without a file (JIT) builds again.
class ModuleSupplier {
public:
    ModuleSupplier(const char* text, uint32_t generation)
        : source_{std::string("r5_player_v") + std::to_string(generation) + ".spp", text},
          generation_(generation)
    {
    }

    spp::Module get()
    {
        if (!path_.empty()) {
            spp::LoadResult again = spp::module_from_library(path_);
            if (again) return std::move(*again.module);
        }
        spp::LoadResult r = backend().load(source_, generation_);
        if (!r) throw std::runtime_error(r.error);
        if (std::filesystem::exists(r.module->origin)) path_ = r.module->origin;
        return std::move(*r.module);
    }

private:
    spp::ScriptSource source_;
    uint32_t generation_;
    std::string path_;
};

ModuleSupplier& v1_supplier()
{
    static ModuleSupplier s(kV1, 1);
    return s;
}

ModuleSupplier& v2_supplier()
{
    static ModuleSupplier s(kV2, 2);
    return s;
}

// The world with its actors. The checker runs on the worker threads.
// It runs after each actor tick. It reads only data that does not change during a frame.
struct Rig {
    Session s;
    std::vector<spp::Instance> actors;
    std::unordered_map<Actor*, spp::ShellBase*> shell_of;  // read only while ticking
    std::atomic<int> violations{0};
    std::atomic<int> ticks_checked{0};

    void start(spp::Module v1)
    {
        ASSERT_NO_FATAL_FAILURE(s.start(std::move(v1)));
        actors.push_back(s.inst);
        const spp::ClassEntry* entry = s.loader.find("Player");
        for (int i = 1; i < kActors; ++i) {
            spp::Instance inst = entry->create();
            actors.push_back(inst);
            s.world.Add(static_cast<Actor*>(inst.native));
        }
        for (const spp::Instance& i : actors)
            shell_of[static_cast<Actor*>(i.native)] = i.shell;
        s.world.SetTickObserver([this](Actor* a) { check(a); });
    }

    // The invariant of one actor after its tick.
    // A half migrated state breaks it. An example is a class record of
    // generation 2 with a block of generation 1. It gives a missing field or a wrong value.
    void check(Actor* a)
    {
        spp::ShellBase* sh = shell_of.find(a)->second;
        ticks_checked.fetch_add(1, std::memory_order_relaxed);
        bool ok = spp::in_script_tick() >= 1;
        const uint32_t gen = sh->cls->generation;
        ok = ok && gen == spp::active_generation;
        const float* hp = spp::field_ptr<float>(sh, "hp");
        const float* health = spp::field_ptr<float>(sh, "health");
        const float* shield = spp::field_ptr<float>(sh, "shield");
        if (gen == 1) {
            ok = ok && hp && !health && !shield && !std::isnan(*hp) && *hp >= kStartHealth;
        } else if (gen == 2) {
            ok = ok && !hp && health && shield && !std::isnan(*health) &&
                 *health >= kStartHealth && *shield == 25.0f;
        } else {
            ok = false;
        }
        if (!ok) violations.fetch_add(1, std::memory_order_relaxed);
    }
};

void run_commit_only_at_barrier(unsigned seed)
{
    Rig rig;
    ASSERT_NO_FATAL_FAILURE(rig.start(v1_supplier().get()));
    Session& s = rig.s;
    std::vector<Actor*> addr = s.world.Actors();
    std::vector<void*> old_state;
    for (const spp::Instance& i : rig.actors) old_state.push_back(i.shell->state);

    // The hook is the frame boundary: it runs on the main thread, at the barrier.
    int frame = 0;         // the number of frames that started
    int commit_frame = 0;  // the frame whose boundary committed (0: none yet)
    int busy_at_commit = 0;
    const uint64_t unsafe0 = spp::unsafe_commit_count();
    s.world.SetFrameBoundaryHook([&] {
        if (s.loader.reload_pending()) {
            busy_at_commit += spp::in_script_tick();
            std::string err;
            if (!s.loader.commit_pending(err)) {
                ADD_FAILURE() << "commit failed: " << err;
                s.commit_failed = true;
            }
            commit_frame = frame;
        }
        s.loader.frame_boundary();
    });

    // The compile thread: it waits for a random time and then requests the reload.
    std::mt19937 rng(seed);
    const auto delay = std::chrono::microseconds(std::uniform_int_distribution<int>(0, 20000)(rng));
    spp::Module v2 = v2_supplier().get();  // built on the main thread, before the threads run
    std::atomic<bool> requested{false};
    std::atomic<bool> stop{false};
    std::atomic<int> unexpected{0};
    bool request_ok = false;
    std::string request_error;
    std::thread compile_thread([&] {
        std::this_thread::sleep_for(delay);
        request_ok = s.loader.request_reload(std::move(v2), request_error);
        requested.store(true);
        // Then it requests a bad reload again and again.
        // So a validation can run at the same time as the commit. Each one must fail.
        while (!stop.load()) {
            spp::LoadResult other = spp::module_from_library(GEN4_BASE_SO);
            std::string e;
            if (other && s.loader.request_reload(std::move(*other.module), e)) ++unexpected;
            (void)s.loader.reload_pending();
            std::this_thread::sleep_for(std::chrono::microseconds(20));
        }
    });

    // At least kFrames frames, then more until the commit happened.
    while (frame < kFrames || (commit_frame == 0 && frame < 100 * kFrames)) {
        ++frame;
        s.world.TickParallel(kDt, kWorkers);
        if (frame >= kFrames && !requested.load()) std::this_thread::yield();
    }
    stop.store(true);
    compile_thread.join();
    EXPECT_EQ(unexpected.load(), 0);
    ASSERT_TRUE(request_ok) << request_error;
    ASSERT_GT(commit_frame, 0) << "the reload did not commit";
    ASSERT_FALSE(s.commit_failed);

    EXPECT_EQ(rig.violations.load(), 0);
    EXPECT_GE(rig.ticks_checked.load(), kActors * kFrames);
    EXPECT_EQ(busy_at_commit, 0);
    EXPECT_EQ(spp::unsafe_commit_count(), unsafe0);
    EXPECT_EQ(spp::in_script_tick(), 0);
    EXPECT_FALSE(s.loader.reload_pending());
    EXPECT_EQ(spp::active_generation, 2u);

    // The last frames: the same behavior for each actor, the same address.
    ASSERT_EQ(s.world.Actors(), addr);
    std::vector<float> x0;
    for (Actor* a : addr) x0.push_back(a->Position.x);
    s.world.TickParallel(kDt, kWorkers);
    ++frame;
    EXPECT_EQ(rig.violations.load(), 0);
    int wrong = 0;
    for (size_t i = 0; i < rig.actors.size(); ++i) {
        spp::ShellBase* sh = rig.actors[i].shell;
        const float* health = spp::field_ptr<float>(sh, "health");
        const float* shield = spp::field_ptr<float>(sh, "shield");
        const float* hp = spp::field_ptr<float>(sh, "hp");
        // The generation 1 Tick ran commit_frame times and added 1 each time.
        const bool good = sh->cls->generation == 2 && sh->state != old_state[i] && !hp &&
                          health && *health == kStartHealth + static_cast<float>(commit_frame) &&
                          shield && *shield == 25.0f &&
                          std::fabs((addr[i]->Position.x - x0[i]) - 2.0f * 5.0f * kDt) < 1e-5f;
        if (!good) ++wrong;
    }
    EXPECT_EQ(wrong, 0);
}

TEST(ParallelTick, CommitOnlyAtBarrier)
{
    for (unsigned seed = 1; seed <= 20; ++seed) {
        SCOPED_TRACE("seed " + std::to_string(seed));
        ASSERT_NO_FATAL_FAILURE(run_commit_only_at_barrier(seed));
    }
}

void run_rejected_reload(unsigned seed)
{
    Rig rig;
    ASSERT_NO_FATAL_FAILURE(rig.start(v1_supplier().get()));
    Session& s = rig.s;
    s.commit_at_frame_boundary();
    std::vector<void*> old_state;
    for (const spp::Instance& i : rig.actors) old_state.push_back(i.shell->state);

    std::mt19937 rng(seed);
    const auto delay = std::chrono::microseconds(std::uniform_int_distribution<int>(0, 20000)(rng));
    std::atomic<bool> requested{false};
    std::string missing_error, base_error, null_error;
    bool missing_ok = true, base_ok = true, null_ok = true, pending_after = true;
    std::thread compile_thread([&] {
        std::this_thread::sleep_for(delay);
        // 1. A library that is not there: there is no Module to give.
        spp::LoadResult missing = spp::module_from_library(GEN3_MISSING_SO);
        missing_ok = static_cast<bool>(missing);
        missing_error = missing.error;
        // 2. A library with another shell layout: the validation rejects it.
        spp::LoadResult other = spp::module_from_library(GEN4_BASE_SO);
        if (other) base_ok = s.loader.request_reload(std::move(*other.module), base_error);
        // 3. A module without the register function.
        null_ok = s.loader.request_reload(spp::Module{}, null_error);
        pending_after = s.loader.reload_pending();
        requested.store(true);
    });

    int frame = 0;
    while (frame < kFrames || !requested.load()) {
        ++frame;
        s.world.TickParallel(kDt, kWorkers);
    }
    compile_thread.join();

    EXPECT_FALSE(missing_ok);
    EXPECT_FALSE(missing_error.empty());
    EXPECT_FALSE(base_ok);
    EXPECT_NE(base_error.find("shell layout"), std::string::npos) << base_error;
    EXPECT_FALSE(null_ok);
    EXPECT_FALSE(null_error.empty());
    EXPECT_FALSE(pending_after);
    EXPECT_FALSE(s.loader.reload_pending());
    EXPECT_FALSE(s.commit_failed);
    EXPECT_EQ(rig.violations.load(), 0);
    EXPECT_EQ(spp::active_generation, 1u);

    // Nothing changed: same blocks, same generation, and the actors kept ticking.
    int wrong = 0;
    for (size_t i = 0; i < rig.actors.size(); ++i) {
        spp::ShellBase* sh = rig.actors[i].shell;
        const float* hp = spp::field_ptr<float>(sh, "hp");
        const bool good = sh->cls->generation == 1 && sh->state == old_state[i] && hp &&
                          *hp == kStartHealth + static_cast<float>(frame);
        if (!good) ++wrong;
    }
    EXPECT_EQ(wrong, 0);

    // The loader still works: a good reload commits at the next barrier.
    std::string error;
    ASSERT_TRUE(s.loader.request_reload(v2_supplier().get(), error)) << error;
    s.world.TickParallel(kDt, kWorkers);
    EXPECT_FALSE(s.commit_failed);
    EXPECT_EQ(spp::active_generation, 2u);
    EXPECT_EQ(rig.violations.load(), 0);
}

TEST(ParallelTick, RejectedReloadFromOtherThread)
{
    for (unsigned seed = 1; seed <= 5; ++seed) {
        SCOPED_TRACE("seed " + std::to_string(seed));
        ASSERT_NO_FATAL_FAILURE(run_rejected_reload(seed));
    }
}

}  // namespace
