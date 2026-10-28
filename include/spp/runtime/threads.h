// The live generation, the thread rules, the stale call guard and the generation pins.
#pragma once
#include <cstdint>

namespace spp {

// The generation that is live now. A generation function checks it.
extern uint32_t active_generation;

// Threads (design sections 6.2 and 6.3).
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
int in_script_tick();
uint64_t unsafe_commit_count();

// RAII: the thread is inside script code while the object lives.
class ScriptTickScope {
public:
    ScriptTickScope();
    ~ScriptTickScope();
    ScriptTickScope(const ScriptTickScope&) = delete;
    ScriptTickScope& operator=(const ScriptTickScope&) = delete;
};

// Called by a generation function when its generation is not live.
void stale_call(const char* what);

// The number of stale calls that the guard ignored since the start.
uint64_t stale_call_count();

// Generation pins (section 4.11). A pin tells the runtime that something outside
// script code (for example a closure in an engine timer) still holds code of the
// generation. The counts are atomic: any thread can pin or unpin. A generation
// number maps to one of 65536 counters; two numbers that are 65536 apart share a
// counter. This can only delay an unload.
void pin(uint32_t generation);
void unpin(uint32_t generation);
uint32_t pin_count(uint32_t generation);

}  // namespace spp
