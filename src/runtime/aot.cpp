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
#include <atomic>
#include <functional>
#include <map>
#include <set>
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

// The registry that the resolvers of the cross-module access use. One AotRegistry for each
// process, as one Loader for each process.
std::atomic<AotRegistry*> g_aot{nullptr};

} // namespace

ClassRecord* AotRegistry::resolve_class(const char* name)
{
    const AotRegistry* r = g_aot.load();
    const ClassEntry* e = r ? r->registry_.find(name) : nullptr;
    return e ? e->record : nullptr;
}

bool AotRegistry::resolve_unit(const char* name, UnitInfo& out)
{
    const AotRegistry* r = g_aot.load();
    if (!r)
        return false;
    const auto it = r->units_.find(name);
    if (it == r->units_.end())
        return false;
    out.fns = it->second.fns.get();
    out.layout = it->second.block.layout;
    out.block = it->second.block.slot ? *it->second.block.slot : nullptr;
    return true;
}

AotRegistry::AotRegistry(const AotTable& table)
{
    active_generation = kAotGeneration;
    // Each script is one unit. An import names a unit of the table, with the interface hash
    // that the importer compiled against. An imported unit registers before its importer.
    std::map<std::string, size_t> by_name;
    for (size_t i = 0; i < table.count; ++i)
        if (!by_name.emplace(table.scripts[i].name, i).second)
            throw std::runtime_error(std::string("two AOT scripts have the name ") + table.scripts[i].name);
    for (size_t i = 0; i < table.count; ++i)
        for (size_t k = 0; k < table.scripts[i].import_count; ++k)
        {
            const AotImport& im = table.scripts[i].imports[k];
            const auto it = by_name.find(im.unit);
            if (it == by_name.end() || table.scripts[it->second].iface_hash != im.hash)
                throw std::runtime_error(std::string("the AOT script ") + table.scripts[i].name + " imports '" + im.unit
                    + "', which " + (it == by_name.end() ? "is not in the AOT table" : "has another interface hash"));
        }
    std::vector<size_t> order;
    std::vector<int> mark(table.count, 0); // 0: new, 1: on the path, 2: done (a cycle keeps the order)
    const std::function<void(size_t)> visit = [&](size_t i)
    {
        if (mark[i] != 0)
            return;
        mark[i] = 1;
        for (size_t k = 0; k < table.scripts[i].import_count; ++k)
            visit(by_name.at(table.scripts[i].imports[k].unit));
        mark[i] = 2;
        order.push_back(i);
    };
    for (size_t i = 0; i < table.count; ++i)
        visit(i);
    // The last AotRegistry gets the resolvers. A process has one AotRegistry at a time.
    g_aot.store(this);
    // An initializer of a module variable can use another unit: the resolvers come first.
    detail::set_class_resolver(&resolve_class);
    detail::set_unit_resolver(&resolve_unit);
    try
    {
        for (size_t i : order)
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
            Unit& unit = units_[script.name];
            unit.fns = std::make_unique<FnTable>();
            unit.fns->entries = reg.fn_table().entries;
            const ModuleStateInfo& ms = reg.module_state();
            if (ms.layout && ms.slot)
            {
                char* block = alloc_state(ms.layout);
                if (!block)
                    throw std::bad_alloc();
                *ms.slot = block; // an initializer can read the block
                unit.block = ms;
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
                    unit.block = {};
                    aligned_free(block); // the partial state is unknown: no destructor call
                    throw;
                }
                module_blocks_.push_back(ms);
            }
            ++script_count_;
        }
        link_parents();
    }
    catch (...)
    {
        unlink();
        throw;
    }
}

// A script parent in another unit: the child record has "super_name" and a slot table with
// holes for the inherited slots. The parent record fills the holes (as Loader::link_first_load).
void AotRegistry::link_parents()
{
    std::set<std::string> done;
    const std::function<void(const std::string&)> link = [&](const std::string& name)
    {
        if (!done.insert(name).second)
            return;
        ClassRecord* r = registry_.find(name)->record;
        if (!r->super_name)
            return;
        const ClassEntry* pe = registry_.find(r->super_name);
        if (!pe)
            throw std::runtime_error("the AOT class " + name + " has the script parent " + r->super_name
                + ", which is not in the AOT table");
        if (layout_hash(pe->record->layout) != r->super_layout_hash)
            throw std::runtime_error("the state layout of the script parent " + std::string(r->super_name)
                + " is not the layout that the AOT class " + name + " compiled against");
        link(r->super_name);
        Linked& l = linked_[name];
        l.own_slots = r->slots;
        l.slots.assign(r->slots, r->slots + r->slot_count);
        const ClassRecord* parent = pe->record;
        for (uint32_t i = 0; i < r->slot_count; ++i)
            if (!l.slots[i] && i < parent->slot_count)
                l.slots[i] = parent->slots[i];
        r->super = pe->record;
        r->slots = l.slots.data();
    };
    for (const auto& [name, e] : registry_.entries())
        link(name);
}

// Puts the records back and frees the module blocks. The destructor and a failed constructor.
void AotRegistry::unlink()
{
    for (auto& [name, l] : linked_)
        if (const ClassEntry* e = registry_.find(name))
        {
            e->record->slots = l.own_slots;
            e->record->super = nullptr;
        }
    linked_.clear();
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
    module_blocks_.clear();
    AotRegistry* self = this;
    if (g_aot.compare_exchange_strong(self, nullptr))
    {
        detail::set_class_resolver(nullptr);
        detail::set_unit_resolver(nullptr);
    }
}

AotRegistry::~AotRegistry()
{
    unlink();
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
