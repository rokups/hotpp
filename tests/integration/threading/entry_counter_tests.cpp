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
#include "Util.h"
#include "hot/backend.h"
#include "support/reload_assert.h"
#include "support/test_support.h"

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

namespace
{

using namespace std::chrono_literals;
using test_support::kDt;
using test_support::Session;

constexpr uint32_t kGenerations = 9; // generation 1 and 8 reloads
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
)") + (even ? "    float shield = 25.0;\n" : "    int spare = 7;\n")
        + (creator ? "    Player() { Util.Probe(this, 1.0); }\n" : "") +
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

hot::Backend& backend()
{
    static std::unique_ptr<hot::Backend> b = hot::make_default_backend();
    return *b;
}

// A new Module of the generation for each call. The first call builds it (about a second).
// A later call loads the library file again. A backend without a file (JIT) builds again.
hot::Module module_of(uint32_t generation, bool creator = false)
{
    static std::vector<std::string> paths(kGenerations + 1);
    static std::vector<std::string> creator_paths(kGenerations + 1);
    std::string& path = (creator ? creator_paths : paths).at(generation);
    if (!path.empty())
    {
        hot::LoadResult again = hot::module_from_library(path);
        if (again)
            return std::move(*again.module);
    }
    hot::ScriptSource src{creator ? "s27_player.hot" : "s21_player.hot", script_text(generation, creator)};
    hot::LoadResult r = backend().load(src, generation);
    if (!r)
        throw std::runtime_error(r.error);
    if (std::filesystem::exists(r.module->origin))
        path = r.module->origin;
    return std::move(*r.module);
}

int32_t int_field(hot::ShellBase* sh, const char* name)
{
    const int32_t* p = hot::field_ptr<int32_t>(sh, name);
    return p ? *p : -1;
}

// The probe runs inside the Tick of a script object: in a counted entry.
std::atomic<int> g_violations{0};
std::atomic<long> g_probes{0};

void check_object(Actor* a)
{
    g_probes.fetch_add(1, std::memory_order_relaxed);
    auto* sh = dynamic_cast<hot::ShellBase*>(a);
    bool ok = sh && hot::entry_depth() == 1 && hot::active_entries() >= 1;
    if (ok)
    {
        const uint32_t gen = sh->cls->generation;
        ok = gen == hot::active_generation && hot::field_ptr<int32_t>(sh, "calls")
            && hot::field_ptr<int32_t>(sh, "hits");
        const float* shield = hot::field_ptr<float>(sh, "shield");
        const int32_t* spare = hot::field_ptr<int32_t>(sh, "spare");
        if (gen % 2 == 0)
            ok = ok && shield && *shield == 25.0f && !spare;
        else
            ok = ok && spare && *spare == 7 && !shield;
    }
    if (!ok)
        g_violations.fetch_add(1, std::memory_order_relaxed);
}

// Aborts the program when a test does not end in time: a deadlock must not hang ctest.
class Watchdog
{
public:
    explicit Watchdog(std::chrono::seconds limit)
        : thread_([this, limit]
    {
        const auto end = std::chrono::steady_clock::now() + limit;
        while (!done_.load())
        {
            if (std::chrono::steady_clock::now() > end)
            {
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

struct Rig
{
    Session s;
    std::vector<hot::Instance> actors;

    void start()
    {
        ASSERT_NO_FATAL_FAILURE(s.start(module_of(1)));
        actors.push_back(s.inst);
        const hot::ClassEntry* entry = s.loader.find("Player");
        for (int i = 1; i < kActors; ++i)
        {
            hot::Instance inst = entry->create();
            actors.push_back(inst);
            s.world.Add(static_cast<Actor*>(inst.native));
        }
    }
};

TEST(EntryCounters, WorkersCallWhileOtherThreadCommits)
{
    // Build all generations first: the build is not part of the test.
    std::vector<hot::Module> mods;
    for (uint32_t g = 2; g <= kGenerations; ++g)
        mods.push_back(module_of(g));
    Rig rig;
    ASSERT_NO_FATAL_FAILURE(rig.start());
    Session& s = rig.s;
    s.loader.set_sync_mode(hot::SyncMode::EntryCounters);
    ASSERT_EQ(s.loader.sync_mode(), hot::SyncMode::EntryCounters);
    g_violations = 0;
    g_probes = 0;
    Util::SetProbe(&check_object);
    Watchdog watchdog(240s);

    std::atomic<bool> stop{false};
    std::vector<long> calls(kActors, 0); // worker w writes the elements i % kWorkers == w
    std::vector<std::thread> workers;
    for (int w = 0; w < kWorkers; ++w)
        workers.emplace_back([&, w]
        {
            while (!stop.load(std::memory_order_relaxed))
                for (int i = w; i < kActors; i += kWorkers)
                {
                    static_cast<Actor*>(rig.actors[i].native)->Tick(kDt);
                    ++calls[i];
                }
        });

    int committed = 0;
    std::string errors;
    const uint64_t windows0 = hot::commit_window_count();
    const uint64_t blocked0 = hot::blocked_entry_count();
    std::thread committer([&]
    {
        for (hot::Module& m : mods)
        {
            // Let the workers run between two commits.
            const long seen = g_probes.load();
            while (g_probes.load() < seen + 4 * kActors)
                std::this_thread::yield();
            if (const hot::ReloadResult req = s.loader.request_reload(std::move(m)); !req)
            {
                errors += "request: " + req.error + "\n";
                continue;
            }
            if (const hot::ReloadResult com = s.loader.commit_pending())
                ++committed;
            else
                errors += "commit: " + com.error + "\n";
            s.loader.frame_boundary();
        }
        const long seen = g_probes.load();
        while (g_probes.load() < seen + 4 * kActors)
            std::this_thread::yield();
    });
    committer.join();
    stop.store(true);
    for (std::thread& t : workers)
        t.join();
    Util::SetProbe(nullptr);

    EXPECT_TRUE(errors.empty()) << errors;
    EXPECT_EQ(committed, static_cast<int>(kGenerations) - 1);
    EXPECT_EQ(hot::commit_window_count() - windows0, kGenerations - 1);
    EXPECT_GT(hot::blocked_entry_count(), blocked0); // the workers ran into the commits
    EXPECT_EQ(hot::active_generation, kGenerations);
    EXPECT_EQ(g_violations.load(), 0);
    EXPECT_EQ(hot::active_entries(), 0u);
    long total = 0;
    int wrong = 0;
    for (int i = 0; i < kActors; ++i)
    {
        hot::ShellBase* sh = rig.actors[i].shell;
        total += calls[i];
        const bool good = sh->cls->generation == kGenerations && int_field(sh, "calls") == calls[i]
            && int_field(sh, "hits") == calls[i];
        if (!good)
            ++wrong;
    }
    EXPECT_EQ(wrong, 0);
    EXPECT_EQ(g_probes.load(), total);
    // The retired generations unload: each frame_boundary of the committer counted a frame.
    s.loader.frame_boundary();
    int retired = 0;
    for (const hot::GenerationInfo& g : s.loader.stats().generations)
        retired += g.state == hot::GenState::Retired;
    EXPECT_EQ(retired, 0) << s.loader.stats_text();
}

// The commit thread for the next tests: it starts while the main thread is in script code.
hot::Loader* g_loader = nullptr;
std::thread* g_commit_thread = nullptr;
hot::ReloadResult g_commit; // the result of the commit that the test starts
uint32_t g_depth_in_probe = 0;
uint32_t g_active_in_probe = 0;

// Starts the commit and waits until the gate is closed. Then the commit waits for this thread.
// The probe returns, and Util::Probe calls TakeDamage: a nested entry while the gate is closed.
void start_commit_and_wait(Actor*)
{
    if (g_commit_thread)
        return;
    g_depth_in_probe = hot::entry_depth();
    g_active_in_probe = hot::active_entries();
    g_commit_thread = new std::thread([] { g_commit = g_loader->commit_pending(); });
    while (!hot::commit_window_closed())
        std::this_thread::yield();
}

TEST(EntryCounters, ReentrantCallDuringCommitDoesNotDeadlock)
{
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(module_of(1)));
    s.loader.set_sync_mode(hot::SyncMode::EntryCounters);
    std::string err;
    ASSERT_RELOAD_OK(s.loader.request_reload(module_of(2)));
    g_loader = &s.loader;
    g_commit_thread = nullptr;
    Util::SetProbe(&start_commit_and_wait);
    Watchdog watchdog(120s);

    s.actor->Tick(kDt); // native -> script Tick -> native probe -> script TakeDamage
    ASSERT_NE(g_commit_thread, nullptr);
    g_commit_thread->join();
    delete g_commit_thread;
    g_commit_thread = nullptr;
    Util::SetProbe(nullptr);

    EXPECT_EQ(g_depth_in_probe, 1u);
    EXPECT_EQ(g_active_in_probe, 1u);
    EXPECT_RELOAD_OK(g_commit);
    EXPECT_EQ(hot::active_generation, 2u);
    EXPECT_EQ(s.inst.shell->cls->generation, 2u);
    EXPECT_EQ(int_field(s.inst.shell, "calls"), 1);
    EXPECT_EQ(int_field(s.inst.shell, "hits"), 1); // the nested call ran before the commit
    s.actor->Tick(kDt); // the next call runs the new generation
    EXPECT_EQ(int_field(s.inst.shell, "calls"), 2);
    EXPECT_NE(hot::field_ptr<float>(s.inst.shell, "shield"), nullptr);
}

void commit_inside_script(Actor*)
{
    g_commit = g_loader->commit_pending();
}

TEST(EntryCounters, CommitInsideScriptCodeFails)
{
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(module_of(1)));
    s.loader.set_sync_mode(hot::SyncMode::EntryCounters);
    std::string err;
    ASSERT_RELOAD_OK(s.loader.request_reload(module_of(2)));
    g_loader = &s.loader;
    g_commit = hot::ReloadResult{hot::ReloadStatus::Applied, hot::ReloadReason::None, {}, {}};
    Util::SetProbe(&commit_inside_script);
    Watchdog watchdog(120s);
    s.actor->Tick(kDt);
    Util::SetProbe(nullptr);
    EXPECT_FALSE(g_commit);
    EXPECT_EQ(g_commit.reason, hot::ReloadReason::Busy);
    EXPECT_NE(g_commit.error.find("inside script code"), std::string::npos) << g_commit.error;
    EXPECT_TRUE(s.loader.reload_pending());
    EXPECT_EQ(hot::active_generation, 1u);
    // Outside script code the same commit works.
    EXPECT_RELOAD_OK(s.loader.commit_pending());
    EXPECT_EQ(hot::active_generation, 2u);
}

void probe_default_mode(Actor*)
{
    g_depth_in_probe = hot::entry_depth();
    g_active_in_probe = hot::active_entries();
}

TEST(EntryCounters, DefaultModeCountsNothing)
{
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(module_of(1)));
    EXPECT_EQ(s.loader.sync_mode(), hot::SyncMode::StopTheWorld);
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
    if (hot::entry_depth() != 1 || hot::active_entries() < 1 || !dynamic_cast<hot::ShellBase*>(a))
        g_unguarded.fetch_add(1, std::memory_order_relaxed);
}

TEST(EntryCounters, CreationThroughTheFactoryIsAnEntry)
{
    constexpr uint32_t kCreatorGenerations = 5;
    std::vector<hot::Module> mods;
    for (uint32_t g = 2; g <= kCreatorGenerations; ++g)
        mods.push_back(module_of(g, true));
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(module_of(1, true)));
    s.loader.set_sync_mode(hot::SyncMode::EntryCounters);
    g_unguarded = 0;
    g_probes = 0;
    Util::SetProbe(&creation_probe);
    Watchdog watchdog(240s);

    const hot::ClassEntry* entry = s.loader.find("Player");
    std::atomic<bool> stop{false};
    std::atomic<long> created{0};
    std::atomic<int> bad{0};
    std::thread creator([&]
    {
        std::deque<hot::Instance> live;
        while (!stop.load(std::memory_order_relaxed))
        {
            hot::Instance inst = entry->create();
            {
                hot::ScriptEntry hold; // a commit cannot run now
                const uint32_t gen = inst.shell->cls->generation;
                const int32_t* hits = hot::field_ptr<int32_t>(inst.shell, "hits");
                const bool spare = hot::field_ptr<int32_t>(inst.shell, "spare") != nullptr;
                const bool shield = hot::field_ptr<float>(inst.shell, "shield") != nullptr;
                // The object has the layout of the generation of its record. The constructor ran once.
                if (gen != hot::active_generation || !hits || *hits != 1 || spare == shield || spare != (gen % 2 == 1))
                    bad.fetch_add(1, std::memory_order_relaxed);
            }
            live.push_back(inst);
            if (live.size() > 32)
            {
                delete static_cast<Actor*>(live.front().native);
                live.pop_front();
            }
            created.fetch_add(1, std::memory_order_relaxed);
        }
        for (hot::Instance& i : live)
            delete static_cast<Actor*>(i.native);
    });

    int committed = 0;
    std::string errors;
    const uint64_t windows0 = hot::commit_window_count();
    std::thread committer([&]
    {
        for (hot::Module& m : mods)
        {
            const long seen = created.load();
            while (created.load() < seen + 200)
                std::this_thread::yield();
            if (const hot::ReloadResult req = s.loader.request_reload(std::move(m)); !req)
            {
                errors += "request: " + req.error + "\n";
                continue;
            }
            if (const hot::ReloadResult com = s.loader.commit_pending())
                ++committed;
            else
                errors += "commit: " + com.error + "\n";
            // No frame_boundary here: it walks the live object table, which the creator changes.
        }
        const long seen = created.load();
        while (created.load() < seen + 200)
            std::this_thread::yield();
    });
    committer.join();
    stop.store(true);
    creator.join();
    Util::SetProbe(nullptr);

    EXPECT_TRUE(errors.empty()) << errors;
    EXPECT_EQ(committed, static_cast<int>(kCreatorGenerations) - 1);
    EXPECT_EQ(hot::commit_window_count() - windows0, kCreatorGenerations - 1);
    EXPECT_EQ(hot::active_generation, kCreatorGenerations);
    EXPECT_EQ(g_unguarded.load(), 0); // each constructor ran inside an entry of depth 1
    EXPECT_GE(g_probes.load(), created.load());
    EXPECT_EQ(bad.load(), 0);
    EXPECT_EQ(hot::active_entries(), 0u);
}

} // namespace
