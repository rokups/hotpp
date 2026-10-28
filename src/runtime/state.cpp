#include "spp/runtime/state.h"

#include "runtime_internal.h"

namespace script {

std::unordered_set<ShellBase*> g_live;

void track_object(ShellBase* self) { g_live.insert(self); }
void untrack_object(ShellBase* self) { g_live.erase(self); }

}  // namespace script
