// Data that the files of the runtime share. This header is not public.
#pragma once
#include <atomic>
#include <cstdint>
#include <unordered_map>

#include "spp/runtime.h"

namespace spp {

// The live objects (state.cpp). The value is the generation whose code made the object with
// "new" (construct_shell), or 0. That code also made the handle of the object, for example the
// control block of a std::shared_ptr. So the generation must stay loaded while the object lives.
extern std::unordered_map<ShellBase*, uint32_t> g_live;

// The count of commits that ran while a thread was in script code (threads.cpp).
extern std::atomic<uint64_t> g_unsafe_commits;

// Fills a new state block (migration.cpp).
void migrate_block(const StateLayout& o, const StateLayout& n, const MigrationPlan& plan,
                   char* from, char* to);

}  // namespace spp
