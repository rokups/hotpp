// The entry counter mode of design 6.3 (S21) with real reloads.
//
// The engine has no frame boundary here. Worker threads call Actor::Tick of their own actors in
// a loop. Another thread requests and commits a chain of reloads at the same time
// (Loader::set_sync_mode(SyncMode::EntryCounters)). Each Tick calls Util::Probe, which is native
// code. The probe checks the object and then calls TakeDamage, a script override.
// So each Tick is script -> native -> script.
//
// The tests check that:
//   - no call is lost and no call runs twice (the fields "calls" and "hits" count them);
//   - no call runs on a half migrated object (the probe checks the record and the fields);
//   - a nested call during a commit that waits does not deadlock;
//   - commit_pending inside script code fails and does not wait for itself;
//   - the default mode counts nothing.
//
// The odd generations have the field "spare", the even generations have "shield".
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "Util.h"
#include "spp/backend.h"
#include "support/test_support.h"

namespace {

using namespace std::chrono_literals;
using test_support::kDt;
using test_support::Session;

constexpr uint32_t kGenerations = 9;  // generation 1 and 8 reloads
constexpr int kActors = 64;
constexpr int kWorkers = 4;

// "creator" adds a constructor. It calls the probe, so the test sees the entry depth of a creation.
std::string script_text(uint32_t generation, bool creator = false)
{
    const bool even = generation % 2 == 0;
    return std::string(R"(
using "Actor.h";
using "Util.h";

class Player : Actor {
    int calls = 0;
    int hits = 0;
)") + (even ? "    float shield = 25.0;\n" : "    int spare = 7;\n") +
           (creator ? "    Player() { Util.Probe(this, 1.0); }\n" : "") +
           R"(
    override void Tick(float dt) {
        this.calls = this.calls + 1;
        Util.Probe(this, 1.0);
    }

    override void TakeDamage(float amount) {
        this.hits = this.hits + 1;
    }
}
)";
}

spp::Backend& backend()
{
    static std::unique_ptr<spp::Backend> b = spp::make_default_backend();
    return *b;
}

// A new Module of the generation for each call. The first call builds it (about a second).
// A later call loads the library file again. A backend without a file (JIT) builds again.
spp::Module module_of(uint32_t generation, bool creator = false)
{
    static std::vector<std::string> paths(kGenerations + 1);
    static std::vector<std::string> creator_paths(kGenerations + 1);
    std::string& path = (creator ? creator_paths : paths).at(generation);
    if (!path.empty()) {
        spp::LoadResult again = spp::module_from_library(path);
        if (again) return std::move(*again.module);
    }
    spp::ScriptSource src{creator ? "s27_player.spp" : "s21_player.spp", script_text(generation, creator)};
    spp::LoadResult r = backend().load(src, generation);
    if (!r) throw std::runtime_error(r.error);
    if (std::filesystem::exists(r.module->origin)) path = r.module->origin;
    return std::move(*r.module);
}

int32_t int_field(spp::ShellBase* sh, const char* name)
{
    const int32_t* p = spp::field_ptr<int32_t>(sh, name);
    return p ? *p : -1;
}

// The probe runs inside the Tick of a script object: in a counted entry.
std::atomic<int> g_violations{0};
std::atomic<long> g_probes{0};

void check_object(Actor* a)
{
    g_probes.fetch_add(1, std::memory_order_relaxed);
    auto* sh = dynamic_cast<spp::ShellBase*>(a);
    bool ok = sh && spp::entry_depth() == 1 && spp::active_entries() >= 1;
    if (ok) {
        const uint32_t gen = sh->cls->generation;
        ok = gen == spp::active_generation && spp::field_ptr<int32_t>(sh, "calls") &&
             spp::field_ptr<int32_t>(sh, "hits");
        const float* shield = spp::field_ptr<float>(sh, "shield");
        const int32_t* spare = spp::field_ptr<int32_t>(sh, "spare");
        if (gen % 2 == 0)
            ok = ok && shield && *shield == 25.0f && !spare;
        else
            ok = ok && spare && *spare == 7 && !shield;
    }
    if (!ok) g_violations.fetch_add(1, std::memory_order_relaxed);
}

// Aborts the program when a test does not end in time: a deadlock must not hang ctest.
class Watchdog {
public:
    explicit Watchdog(std::chrono::seconds limit)
        : thread_([this, limit] {
              const auto end = std::chrono::steady_clock::now() + limit;
              while (!done_.load()) {
                  if (std::chrono::steady_clock::now() > end) {
                      std::fprintf(stderr, "watchdog: the test did not end; deadlock\n");
                      std::abort();
                  }
                  std::this_thread::sleep_for(10ms);
              }
          })
    {
    }
    ~Watchdog()
    {
        done_.store(true);
        thread_.join();
    }

private:
    std::atomic<bool> done_{false};
    std::thread thread_;
};

struct Rig {
    Session s;
    std::vector<spp::Instance> actors;

    void start()
    {
        ASSERT_NO_FATAL_FAILURE(s.start(module_of(1)));
        actors.push_back(s.inst);
        const spp::ClassEntry* entry = s.loader.find("Player");
        for (int i = 1; i < kActors; ++i) {
            spp::Instance inst = entry->create();
            actors.push_back(inst);
            s.world.Add(static_cast<Actor*>(inst.native));
        }
    }
};

TEST(EntryCounters, WorkersCallWhileOtherThreadCommits)
{
    // Build all generations first: the build is not part of the test.
    std::vector<spp::Module> mods;
    for (uint32_t g = 2; g <= kGenerations; ++g) mods.push_back(module_of(g));
    Rig rig;
    ASSERT_NO_FATAL_FAILURE(rig.start());
    Session& s = rig.s;
    s.loader.set_sync_mode(spp::SyncMode::EntryCounters);
    ASSERT_EQ(s.loader.sync_mode(), spp::SyncMode::EntryCounters);
    g_violations = 0;
    g_probes = 0;
    Util::SetProbe(&check_object);
    Watchdog watchdog(240s);

    std::atomic<bool> stop{false};
    std::vector<long> calls(kActors, 0);  // worker w writes the elements i % kWorkers == w
    std::vector<std::thread> workers;
    for (int w = 0; w < kWorkers; ++w)
        workers.emplace_back([&, w] {
            while (!stop.load(std::memory_order_relaxed))
                for (int i = w; i < kActors; i += kWorkers) {
                    static_cast<Actor*>(rig.actors[i].native)->Tick(kDt);
                    ++calls[i];
                }
        });

    int committed = 0;
    std::string errors;
    const uint64_t windows0 = spp::commit_window_count();
    const uint64_t blocked0 = spp::blocked_entry_count();
    std::thread committer([&] {
        for (spp::Module& m : mods) {
            // Let the workers run between two commits.
            const long seen = g_probes.load();
            while (g_probes.load() < seen + 4 * kActors) std::this_thread::yield();
            std::string err;
            if (!s.loader.request_reload(std::move(m), err)) {
                errors += "request: " + err + "\n";
                continue;
            }
            if (s.loader.commit_pending(err))
                ++committed;
            else
                errors += "commit: " + err + "\n";
            s.loader.frame_boundary();
        }
        const long seen = g_probes.load();
        while (g_probes.load() < seen + 4 * kActors) std::this_thread::yield();
    });
    committer.join();
    stop.store(true);
    for (std::thread& t : workers) t.join();
    Util::SetProbe(nullptr);

    EXPECT_TRUE(errors.empty()) << errors;
    EXPECT_EQ(committed, static_cast<int>(kGenerations) - 1);
    EXPECT_EQ(spp::commit_window_count() - windows0, kGenerations - 1);
    EXPECT_GT(spp::blocked_entry_count(), blocked0);  // the workers ran into the commits
    EXPECT_EQ(spp::active_generation, kGenerations);
    EXPECT_EQ(g_violations.load(), 0);
    EXPECT_EQ(spp::active_entries(), 0u);
    long total = 0;
    int wrong = 0;
    for (int i = 0; i < kActors; ++i) {
        spp::ShellBase* sh = rig.actors[i].shell;
        total += calls[i];
        const bool good = sh->cls->generation == kGenerations && int_field(sh, "calls") == calls[i] &&
                          int_field(sh, "hits") == calls[i];
        if (!good) ++wrong;
    }
    EXPECT_EQ(wrong, 0);
    EXPECT_EQ(g_probes.load(), total);
    // The retired generations unload: each frame_boundary of the committer counted a frame.
    s.loader.frame_boundary();
    int retired = 0;
    for (const spp::GenerationInfo& g : s.loader.stats().generations) retired += g.state == spp::GenState::Retired;
    EXPECT_EQ(retired, 0) << s.loader.stats_text();
}

// The commit thread for the next tests: it starts while the main thread is in script code.
spp::Loader* g_loader = nullptr;
std::thread* g_commit_thread = nullptr;
bool g_commit_ok = false;
std::string g_commit_error;
uint32_t g_depth_in_probe = 0;
uint32_t g_active_in_probe = 0;

// Starts the commit and waits until the gate is closed. Then the commit waits for this thread.
// The probe returns, and Util::Probe calls TakeDamage: a nested entry while the gate is closed.
void start_commit_and_wait(Actor*)
{
    if (g_commit_thread) return;
    g_depth_in_probe = spp::entry_depth();
    g_active_in_probe = spp::active_entries();
    g_commit_thread = new std::thread([] { g_commit_ok = g_loader->commit_pending(g_commit_error); });
    while (!spp::commit_window_closed()) std::this_thread::yield();
}

TEST(EntryCounters, ReentrantCallDuringCommitDoesNotDeadlock)
{
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(module_of(1)));
    s.loader.set_sync_mode(spp::SyncMode::EntryCounters);
    std::string err;
    ASSERT_TRUE(s.loader.request_reload(module_of(2), err)) << err;
    g_loader = &s.loader;
    g_commit_thread = nullptr;
    Util::SetProbe(&start_commit_and_wait);
    Watchdog watchdog(120s);

    s.actor->Tick(kDt);  // native -> script Tick -> native probe -> script TakeDamage
    ASSERT_NE(g_commit_thread, nullptr);
    g_commit_thread->join();
    delete g_commit_thread;
    g_commit_thread = nullptr;
    Util::SetProbe(nullptr);

    EXPECT_EQ(g_depth_in_probe, 1u);
    EXPECT_EQ(g_active_in_probe, 1u);
    EXPECT_TRUE(g_commit_ok) << g_commit_error;
    EXPECT_EQ(spp::active_generation, 2u);
    EXPECT_EQ(s.inst.shell->cls->generation, 2u);
    EXPECT_EQ(int_field(s.inst.shell, "calls"), 1);
    EXPECT_EQ(int_field(s.inst.shell, "hits"), 1);  // the nested call ran before the commit
    s.actor->Tick(kDt);  // the next call runs the new generation
    EXPECT_EQ(int_field(s.inst.shell, "calls"), 2);
    EXPECT_NE(spp::field_ptr<float>(s.inst.shell, "shield"), nullptr);
}

void commit_inside_script(Actor*)
{
    g_commit_ok = g_loader->commit_pending(g_commit_error);
}

TEST(EntryCounters, CommitInsideScriptCodeFails)
{
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(module_of(1)));
    s.loader.set_sync_mode(spp::SyncMode::EntryCounters);
    std::string err;
    ASSERT_TRUE(s.loader.request_reload(module_of(2), err)) << err;
    g_loader = &s.loader;
    g_commit_ok = true;
    g_commit_error.clear();
    Util::SetProbe(&commit_inside_script);
    Watchdog watchdog(120s);
    s.actor->Tick(kDt);
    Util::SetProbe(nullptr);
    EXPECT_FALSE(g_commit_ok);
    EXPECT_NE(g_commit_error.find("inside script code"), std::string::npos) << g_commit_error;
    EXPECT_TRUE(s.loader.reload_pending());
    EXPECT_EQ(spp::active_generation, 1u);
    // Outside script code the same commit works.
    EXPECT_TRUE(s.loader.commit_pending(err)) << err;
    EXPECT_EQ(spp::active_generation, 2u);
}

void probe_default_mode(Actor*)
{
    g_depth_in_probe = spp::entry_depth();
    g_active_in_probe = spp::active_entries();
}

TEST(EntryCounters, DefaultModeCountsNothing)
{
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(module_of(1)));
    EXPECT_EQ(s.loader.sync_mode(), spp::SyncMode::StopTheWorld);
    g_depth_in_probe = 99;
    g_active_in_probe = 99;
    Util::SetProbe(&probe_default_mode);
    s.actor->Tick(kDt);
    Util::SetProbe(nullptr);
    EXPECT_EQ(g_depth_in_probe, 0u);
    EXPECT_EQ(g_active_in_probe, 0u);
    EXPECT_EQ(int_field(s.inst.shell, "hits"), 1);
}

// S27: the creation of an object through the factory is an entry. A thread creates objects while
// another thread commits reloads. The constructor calls the probe: it sees the entry depth of the
// creation. The creator checks each new object while it holds an entry, so a commit cannot
// migrate the object under the check.
std::atomic<int> g_unguarded{0};

void creation_probe(Actor* a)
{
    g_probes.fetch_add(1, std::memory_order_relaxed);
    if (spp::entry_depth() != 1 || spp::active_entries() < 1 || !dynamic_cast<spp::ShellBase*>(a))
        g_unguarded.fetch_add(1, std::memory_order_relaxed);
}

TEST(EntryCounters, CreationThroughTheFactoryIsAnEntry)
{
    constexpr uint32_t kCreatorGenerations = 5;
    std::vector<spp::Module> mods;
    for (uint32_t g = 2; g <= kCreatorGenerations; ++g) mods.push_back(module_of(g, true));
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(module_of(1, true)));
    s.loader.set_sync_mode(spp::SyncMode::EntryCounters);
    g_unguarded = 0;
    g_probes = 0;
    Util::SetProbe(&creation_probe);
    Watchdog watchdog(240s);

    const spp::ClassEntry* entry = s.loader.find("Player");
    std::atomic<bool> stop{false};
    std::atomic<long> created{0};
    std::atomic<int> bad{0};
    std::thread creator([&] {
        std::deque<spp::Instance> live;
        while (!stop.load(std::memory_order_relaxed)) {
            spp::Instance inst = entry->create();
            {
                spp::ScriptEntry hold;  // a commit cannot run now
                const uint32_t gen = inst.shell->cls->generation;
                const int32_t* hits = spp::field_ptr<int32_t>(inst.shell, "hits");
                const bool spare = spp::field_ptr<int32_t>(inst.shell, "spare") != nullptr;
                const bool shield = spp::field_ptr<float>(inst.shell, "shield") != nullptr;
                // The object has the layout of the generation of its record. The constructor ran once.
                if (gen != spp::active_generation || !hits || *hits != 1 || spare == shield || spare != (gen % 2 == 1))
                    bad.fetch_add(1, std::memory_order_relaxed);
            }
            live.push_back(inst);
            if (live.size() > 32) {
                delete static_cast<Actor*>(live.front().native);
                live.pop_front();
            }
            created.fetch_add(1, std::memory_order_relaxed);
        }
        for (spp::Instance& i : live) delete static_cast<Actor*>(i.native);
    });

    int committed = 0;
    std::string errors;
    const uint64_t windows0 = spp::commit_window_count();
    std::thread committer([&] {
        for (spp::Module& m : mods) {
            const long seen = created.load();
            while (created.load() < seen + 200) std::this_thread::yield();
            std::string err;
            if (!s.loader.request_reload(std::move(m), err)) {
                errors += "request: " + err + "\n";
                continue;
            }
            if (s.loader.commit_pending(err))
                ++committed;
            else
                errors += "commit: " + err + "\n";
            // No frame_boundary here: it walks the live object table, which the creator changes.
        }
        const long seen = created.load();
        while (created.load() < seen + 200) std::this_thread::yield();
    });
    committer.join();
    stop.store(true);
    creator.join();
    Util::SetProbe(nullptr);

    EXPECT_TRUE(errors.empty()) << errors;
    EXPECT_EQ(committed, static_cast<int>(kCreatorGenerations) - 1);
    EXPECT_EQ(spp::commit_window_count() - windows0, kCreatorGenerations - 1);
    EXPECT_EQ(spp::active_generation, kCreatorGenerations);
    EXPECT_EQ(g_unguarded.load(), 0);  // each constructor ran inside an entry of depth 1
    EXPECT_GE(g_probes.load(), created.load());
    EXPECT_EQ(bad.load(), 0);
    EXPECT_EQ(spp::active_entries(), 0u);
}

}  // namespace
