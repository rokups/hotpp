#include "spp/runtime/threads.h"

#include <atomic>
#include <cstdio>

#include "runtime_internal.h"

namespace spp {

uint32_t active_generation = 0;

// The debug check of the safe point (see "Threads" in runtime/threads.h).
static std::atomic<int> g_in_script_tick{0};
std::atomic<uint64_t> g_unsafe_commits{0};

#ifndef NDEBUG
int in_script_tick() { return g_in_script_tick.load(); }
uint64_t unsafe_commit_count() { return g_unsafe_commits.load(); }
ScriptTickScope::ScriptTickScope() { g_in_script_tick.fetch_add(1); }
ScriptTickScope::~ScriptTickScope() { g_in_script_tick.fetch_sub(1); }
#endif

static std::atomic<uint64_t> g_stale_calls{0};

uint64_t stale_call_count() { return g_stale_calls.load(std::memory_order_relaxed); }

void stale_call(const char* what)
{
    g_stale_calls.fetch_add(1, std::memory_order_relaxed);
    std::fprintf(stderr, "[script] stale call ignored: %s (active generation %u)\n", what,
                 active_generation);
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

}  // namespace spp
