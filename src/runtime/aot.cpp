// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The AOT runtime (design M7): the AOT registry and the object tracking of a
// program that has no loader. It does not include loader.h.
#include "hot/aot.h"

#include "hot/runtime/iface.h"
#include "hot/runtime/shell.h"
#include "hot/runtime/state.h"
#include "hot/runtime/threads.h"

#include <algorithm>
#include <stdexcept>

namespace hot
{

// An AOT program never reloads. Nobody reads the list of live objects, so the calls do nothing.
void track_object(ShellBase*)
{
}
void untrack_object(ShellBase*)
{
}

namespace
{

// The generation number of all AOT code. The guard of each generation function reads it.
constexpr uint32_t kAotGeneration = 1;

void destroy_module_block(const ModuleStateInfo& info)
{
    if (!info.slot || !*info.slot)
        return;
    char* block = static_cast<char*>(*info.slot);
    destroy_fields(info.layout, block);
    aligned_free(block);
    *info.slot = nullptr;
}

} // namespace

AotRegistry::AotRegistry(const AotTable& table)
{
    active_generation = kAotGeneration;
    try
    {
        for (size_t i = 0; i < table.count; ++i)
        {
            const AotScript& script = table.scripts[i];
            Registry reg;
            script.register_fn(reg);
            if (reg.live_flag())
                *reg.live_flag() = 1; // all AOT code is live
            for (const auto& [name, e] : reg.entries())
            {
                if (registry_.find(name))
                    throw std::runtime_error("the class " + name + " is in two AOT scripts (" + script.name + ")");
                registry_.add(e.record, e.create);
                // The same table as the loader builds (Loader::adopt_ifaces). Without it, a call
                // of an interface method finds no slot.
                std::vector<uint32_t> table = build_iface_table(e.record->iface_rows);
                if (!table.empty())
                {
                    std::vector<uint32_t>& owned = iface_slots_[name];
                    owned = std::move(table);
                    e.record->iface_slots = owned.data();
                    e.record->iface_slot_count = static_cast<uint32_t>(owned.size());
                }
            }
            for (const auto& [key, t] : reg.thunks())
                registry_.add_thunk(t);
            const ModuleStateInfo& ms = reg.module_state();
            if (ms.layout && ms.slot)
            {
                char* block = alloc_state(ms.layout);
                if (!block)
                    throw std::bad_alloc();
                *ms.slot = block; // an initializer can read the block
                try
                {
                    for (uint32_t f = 0; f < ms.layout->field_count; ++f)
                    {
                        const FieldInfo& fi = ms.layout->fields[f];
                        if (fi.init)
                            fi.init(block, block + fi.offset);
                    }
                }
                catch (...)
                {
                    *ms.slot = nullptr;
                    aligned_free(block); // the partial state is unknown: no destructor call
                    throw;
                }
                module_blocks_.push_back(ms);
            }
            ++script_count_;
        }
    }
    catch (...)
    {
        for (const ModuleStateInfo& info : module_blocks_)
            destroy_module_block(info);
        module_blocks_.clear();
        throw;
    }
}

AotRegistry::~AotRegistry()
{
    // The tables go with this object. A record is static: another registry may have set its own
    // table later. Clear a pointer only when it still points into the table of this registry.
    for (const auto& [name, e] : registry_.entries())
    {
        auto own = iface_slots_.find(name);
        if (e.record && e.record->iface_slots && own != iface_slots_.end()
            && e.record->iface_slots == own->second.data())
        {
            e.record->iface_slots = nullptr;
            e.record->iface_slot_count = 0;
        }
    }
    for (auto it = module_blocks_.rbegin(); it != module_blocks_.rend(); ++it)
        destroy_module_block(*it);
}

std::vector<std::string> AotRegistry::class_names() const
{
    std::vector<std::string> names;
    for (const auto& [name, e] : registry_.entries())
        names.push_back(name);
    std::sort(names.begin(), names.end());
    return names;
}

const ClassRecord* AotRegistry::record(const std::string& class_name) const
{
    const ClassEntry* e = registry_.find(class_name);
    return e ? e->record : nullptr;
}

Instance AotRegistry::create(const std::string& class_name) const
{
    const ClassEntry* e = registry_.find(class_name);
    if (!e)
        throw std::runtime_error("no AOT script class " + class_name);
    return e->create();
}

} // namespace hot
