// The live generation, the thread rules, the stale call guard and the generation pins.
#pragma once
#include <atomic>
#include <cstdint>

#if defined(_MSC_VER) && !defined(__clang__)
    // unverified: not built on MSVC. This header declares the intrinsics of the atomic helpers.
    #include <intrin.h>
#endif

#include "hot/runtime/compat.h"
#include "hot/runtime/hidden.h"

namespace hot
{

// The generation that is live now. A generation function checks it.
extern uint32_t active_generation;

// ---------------------------------------------------------------- synchronization modes
// The two modes of design 6.3. Loader::set_sync_mode selects one at run time.
//
// StopTheWorld (the default). The engine commits at a frame boundary, when no thread is in
// script code (see "Threads in StopTheWorld mode" below). A script entry pays one relaxed load
// of a global flag and one branch that is not taken.
//
// EntryCounters. For an engine with no frame boundary. Each script entry (a slot call through
// hot::call, a thunk, a closure call, a script destructor) is a ScriptEntry. The outermost entry
// of a thread increments one global count of active entries. A nested entry on the same thread
// (script -> native -> script) only increments a thread-local depth, so it never blocks.
// A commit (CommitWindow) closes the gate and waits until the count is zero. A new outermost
// entry blocks at the closed gate. When the commit ends, the gate opens and the blocked threads
// go on in the new generation.
//
// Limits of EntryCounters:
//   - A deadlock occurs when script code waits for a thread that is blocked at the gate,
//     or for the end of the commit. The commit waits for that script code.
//   - commit_pending on a thread that is in script code fails. It does not wait for itself.
//   - A thread that entered script code in StopTheWorld mode is not counted.
//     Change the mode only when no thread is in script code.
//   - The creation through the factory (ClassEntry::create) is an entry. Native code that
//     reads the fields of a script object holds a ScriptEntry. With HOT_THREAD_SAFE=0, object
//     creation and destruction stay on one thread at a time (the rule of the live object table).
//
// Define HOT_NO_ENTRY_COUNTERS to remove the mode at compile time. Then a ScriptEntry is empty.
enum class SyncMode : uint8_t
{
    StopTheWorld,
    EntryCounters,
};

#ifdef HOT_NO_ENTRY_COUNTERS
inline constexpr bool kEntryCounters = false;
#else
inline constexpr bool kEntryCounters = true;
#endif

namespace detail
{
// True in EntryCounters mode. Loader::set_sync_mode writes it.
// A plain bool: the accessors below use atomic builtins, so no std frame appears in the debugger.
extern bool g_entry_counters;
HOT_HIDDEN bool load_entry_counters()
{
#if defined(_MSC_VER) && !defined(__clang__)
    // unverified: not built on MSVC. A volatile load of one byte, then a compiler barrier.
    const bool value = __iso_volatile_load8(reinterpret_cast<const volatile char*>(&g_entry_counters)) != 0;
    _ReadWriteBarrier();
    return value;
#else
    return __atomic_load_n(&g_entry_counters, __ATOMIC_RELAXED);
#endif
}
HOT_HIDDEN void store_entry_counters(bool value)
{
#if defined(_MSC_VER) && !defined(__clang__)
    // unverified: not built on MSVC. An interlocked exchange of one byte is a full barrier.
    _InterlockedExchange8(reinterpret_cast<volatile char*>(&g_entry_counters), value ? 1 : 0);
#else
    __atomic_store_n(&g_entry_counters, value, __ATOMIC_SEQ_CST);
#endif
}
HOT_NODEBUG void enter_script(); // the slow path of ScriptEntry (threads.cpp)
HOT_NODEBUG void leave_script();
} // namespace detail

// True when the process uses the EntryCounters mode.
HOT_HIDDEN bool entry_counters_enabled()
{
    if constexpr (kEntryCounters)
        return detail::load_entry_counters();
    else
        return false;
}

// RAII: one script entry. In StopTheWorld mode it does nothing after the flag check.
// The constructor can block while a commit runs (EntryCounters mode).
class ScriptEntry
{
public:
    HOT_HIDDEN ScriptEntry()
        : counted_(entry_counters_enabled())
    {
        if (HOT_UNLIKELY(counted_))
            detail::enter_script();
    }
    HOT_HIDDEN ~ScriptEntry()
    {
        if (HOT_UNLIKELY(counted_))
            detail::leave_script();
    }
    ScriptEntry(const ScriptEntry&) = delete;
    ScriptEntry& operator=(const ScriptEntry&) = delete;

private:
    bool counted_;
};

// The number of threads in a counted script entry now (EntryCounters mode).
uint32_t active_entries();
// The depth of counted script entries on the calling thread. A commit window counts as one.
uint32_t entry_depth();
// True while a commit window is closed (from the start of the wait to the end of the commit).
bool commit_window_closed();
// The number of commit windows since the program start.
uint64_t commit_window_count();
// The number of entries that waited at a closed gate since the program start.
uint64_t blocked_entry_count();

// RAII: a commit window (EntryCounters mode). The constructor closes the gate and waits until
// no thread is in a counted entry. Then the calling thread is the only thread in script code.
// It can run script code (for example the onReload hooks): its depth is one.
// The destructor opens the gate. Only one window is open at a time; a second one waits.
// The caller must not be in a counted script entry (entry_depth() == 0), else it waits for ever.
// Loader::commit_pending uses it. An engine needs it only for its own changes of script state.
class CommitWindow
{
public:
    CommitWindow();
    ~CommitWindow();
    CommitWindow(const CommitWindow&) = delete;
    CommitWindow& operator=(const CommitWindow&) = delete;
};

// Threads in StopTheWorld mode (design sections 6.2 and 6.3).
//
// The engine can run script code on worker threads.
// A reload commit changes the class records, the state blocks and active_generation.
// So the commit must run when no thread is in script code.
// The engine gives this guarantee. The commit runs only in the frame boundary hook.
// The hook runs on the main thread, after all workers reached the barrier.
// The workers restart after the hook. The barrier also orders the memory:
// the workers see the writes of the commit after they restart.
//
// The counter below is a debug check of that guarantee.
// The engine wraps each entry into script code in a ScriptTickScope.
// commit_pending() reads the counter. A value other than zero is an engine bug.
// The commit counts it (unsafe_commit_count) and then asserts.
//
// Which thread may touch which data:
//   - Worker threads read class records, state blocks and active_generation,
//     only inside a ScriptTickScope. They write the state block of their own actors.
//     They do not create or destroy script objects. They do not call the Loader.
//   - The main thread creates and destroys script objects. It runs the frame
//     boundary hook (commit_pending, frame_boundary, stats) and calls Loader::load.
//   - Other threads (for example a compile thread) call only
//     Loader::request_reload and Loader::reload_pending. These take the Loader mutex.
//   - Pins and the stale call counter are atomic. Any thread can use them.
//
// The check exists in debug builds only (design 6.3). With NDEBUG, kTickChecks is false,
// ScriptTickScope does nothing, in_script_tick() is always zero and the commit does not
// check the depths. So a release build pays nothing.
// In EntryCounters mode the commit does not check this counter: a worker can wait at the
// gate inside a ScriptTickScope.
#ifdef NDEBUG
inline constexpr bool kTickChecks = false;
inline int in_script_tick()
{
    return 0;
}
inline uint64_t unsafe_commit_count()
{
    return 0;
}
class ScriptTickScope
{
public:
    ScriptTickScope() = default;
    ScriptTickScope(const ScriptTickScope&) = delete;
    ScriptTickScope& operator=(const ScriptTickScope&) = delete;
};
#else
inline constexpr bool kTickChecks = true;
int in_script_tick();
uint64_t unsafe_commit_count();

// RAII: the thread is inside script code while the object lives.
class ScriptTickScope
{
public:
    ScriptTickScope();
    ~ScriptTickScope();
    ScriptTickScope(const ScriptTickScope&) = delete;
    ScriptTickScope& operator=(const ScriptTickScope&) = delete;
};
#endif

// Called by a generation function when its generation is not live.
HOT_NODEBUG void stale_call(const char* what);

// The number of stale calls that the guard ignored since the start.
uint64_t stale_call_count();

// Generation pins (section 4.11). A pin tells the runtime that something outside
// script code (for example a closure in an engine timer) still holds code of the
// generation. The counts are atomic: any thread can pin or unpin. A generation
// number maps to one of 65536 counters; two numbers that are 65536 apart share a
// counter. This can only delay an unload.
HOT_NODEBUG void pin(uint32_t generation);
HOT_NODEBUG void unpin(uint32_t generation);
uint32_t pin_count(uint32_t generation);

} // namespace hot
