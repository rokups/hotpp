#include "spp/runtime/threads.h"

#include "runtime_internal.h"

#include <atomic>
#include <cstdio>
#include <mutex>

namespace spp
{

uint32_t active_generation = 0;

// The debug check of the safe point (see "Threads" in runtime/threads.h).
static std::atomic<int> g_in_script_tick{0};
std::atomic<uint64_t> g_unsafe_commits{0};

#ifndef NDEBUG
int in_script_tick()
{
    return g_in_script_tick.load();
}
uint64_t unsafe_commit_count()
{
    return g_unsafe_commits.load();
}
ScriptTickScope::ScriptTickScope()
{
    g_in_script_tick.fetch_add(1);
}
ScriptTickScope::~ScriptTickScope()
{
    g_in_script_tick.fetch_sub(1);
}
#endif

// ---------------------------------------------------------------- entry counters (design 6.3)
// The protocol. All operations on g_active and g_gate are sequentially consistent.
//   Entry: increment g_active, then read g_gate. Open: the thread is in. Closed: decrement
//          g_active, wake the commit when the count is zero, wait until the gate opens, retry.
//   Commit: close g_gate, then wait until g_active is zero.
// The total order of the seq_cst operations gives: an entry sees the closed gate, or the
// commit sees the count of the entry. Never neither. The decrement of a leave is a release.
// The commit reads zero with acquire, so all script accesses happen before the commit.
// The entry reads the open gate with acquire after the release of the commit end, so the
// commit happens before the entry.
namespace detail
{
bool g_entry_counters = false;
}

namespace
{
alignas(64) std::atomic<uint32_t> g_active{0}; // threads in a counted entry
alignas(64) std::atomic<uint32_t> g_gate{0}; // 1: a commit window is closed
std::atomic<uint64_t> g_windows{0};
std::atomic<uint64_t> g_blocked{0}; // entries that waited at a closed gate
std::mutex g_window_mutex; // one commit window at a time
thread_local uint32_t t_depth = 0;

void leave_outermost()
{
    // The last thread out wakes a commit that waits.
    if (g_active.fetch_sub(1) == 1 && g_gate.load() != 0)
        g_active.notify_all();
}
} // namespace

void detail::enter_script()
{
    if (t_depth++ != 0)
        return; // a nested entry: the thread is in already
    for (;;)
    {
        g_active.fetch_add(1);
        if (g_gate.load() == 0)
            return;
        leave_outermost(); // a commit runs: back off, wait, and try again
        g_blocked.fetch_add(1, std::memory_order_relaxed);
        g_gate.wait(1);
    }
}

void detail::leave_script()
{
    if (--t_depth != 0)
        return;
    leave_outermost();
}

uint32_t active_entries()
{
    return g_active.load();
}
uint32_t entry_depth()
{
    return t_depth;
}
bool commit_window_closed()
{
    return g_gate.load() != 0;
}
uint64_t commit_window_count()
{
    return g_windows.load(std::memory_order_relaxed);
}
uint64_t blocked_entry_count()
{
    return g_blocked.load(std::memory_order_relaxed);
}

CommitWindow::CommitWindow()
{
    g_window_mutex.lock();
    g_gate.store(1);
    for (uint32_t n; (n = g_active.load()) != 0;)
        g_active.wait(n);
    ++t_depth; // the commit thread may run script code, for example a hook
}

CommitWindow::~CommitWindow()
{
    --t_depth;
    g_windows.fetch_add(1, std::memory_order_relaxed);
    g_gate.store(0);
    g_gate.notify_all();
    g_window_mutex.unlock();
}

static std::atomic<uint64_t> g_stale_calls{0};

uint64_t stale_call_count()
{
    return g_stale_calls.load(std::memory_order_relaxed);
}

void stale_call(const char* what)
{
    g_stale_calls.fetch_add(1, std::memory_order_relaxed);
    std::fprintf(stderr, "[script] stale call ignored: %s (active generation %u)\n", what, active_generation);
}

// ---------------------------------------------------------------- pins
static constexpr uint32_t kPinSlots = 1u << 16;
static std::atomic<uint32_t> g_pins[kPinSlots];

void pin(uint32_t generation)
{
    g_pins[generation % kPinSlots].fetch_add(1, std::memory_order_relaxed);
}

void unpin(uint32_t generation)
{
    // Release: all uses of the module code before the unpin happen before the unload.
    g_pins[generation % kPinSlots].fetch_sub(1, std::memory_order_release);
}

uint32_t pin_count(uint32_t generation)
{
    return g_pins[generation % kPinSlots].load(std::memory_order_acquire);
}

} // namespace spp
