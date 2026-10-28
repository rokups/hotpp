// Data that the files of the runtime share. This header is not public.
#pragma once
#include <atomic>
#include <unordered_set>

#include "spp/runtime.h"

namespace spp {

// The live objects (state.cpp).
extern std::unordered_set<ShellBase*> g_live;

// The count of commits that ran while a thread was in script code (threads.cpp).
extern std::atomic<uint64_t> g_unsafe_commits;

// Fills a new state block (migration.cpp).
void migrate_block(const StateLayout& o, const StateLayout& n, const MigrationPlan& plan,
                   char* from, char* to);

}  // namespace spp
