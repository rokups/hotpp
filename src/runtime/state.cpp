#include "spp/runtime/state.h"

#include "runtime_internal.h"

namespace spp
{

std::unordered_map<ShellBase*, uint32_t> g_live;

void track_object(ShellBase* self)
{
    g_live.emplace(self, 0u);
}
void untrack_object(ShellBase* self)
{
    g_live.erase(self);
}

void note_creator(ShellBase* self, uint32_t generation)
{
    auto it = g_live.find(self);
    if (it != g_live.end())
        it->second = generation;
}

} // namespace spp
