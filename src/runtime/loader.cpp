// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

#include "hot/runtime/loader.h"

#include "hot/backend.h"
#include "hot/runtime/iface.h"

#include "runtime_internal.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <exception>
#include <stdexcept>
#include <functional>
#include <iterator>
#include <map>
#include <set>
#include <utility>

namespace hot
{

struct PendingClass
{
    ClassEntry* canon; // null: the class is new
    ClassRecord* incoming;
    Factory create;
    MigrationPlan plan;
};

// One module of a Pending: one unit.
struct PendingUnit
{
    Module module; // keeps the code alive; released with the Pending when the reload fails
    Registry staged;
    std::vector<PendingClass> classes;
    MigrationPlan mod_plan; // the plan of the module state block (old layout to new layout)
    // The core instance (module.h) when the generation adds a class or a thunk, else empty.
    bool have_core = false;
    Module core;
    Registry core_reg;
};

// A staged reload: one or more units that one commit makes live.
struct Loader::Pending
{
    std::vector<PendingUnit> units;
};

namespace
{
// The layout of a script without module variables: the plan of a new block is all "Init".
// A removed class also gets it when the generation of its last layout unloads.
const StateLayout kEmptyLayout = {"", 1, 1, 0, nullptr};

// The name of the script parent of "r", in this unit or in another unit, or "".
const char* parent_name(const ClassRecord* r)
{
    return r->super ? r->super->name : r->super_name ? r->super_name : "";
}

// Copies the parts of a generation record that change at a reload into the canonical record.
void adopt(ClassRecord* canon, ClassRecord* in)
{
    canon->slots = in->slots;
    canon->slot_count = in->slot_count;
    canon->layout = in->layout;
    canon->generation = in->generation;
    canon->on_reload = in->on_reload;
    canon->new_default = in->new_default;
    canon->is_abstract = in->is_abstract;
    canon->dtor = in->dtor;
    canon->attributes = in->attributes;
    canon->slot_names = in->slot_names;
    canon->ctors = in->ctors;
    if (in != canon)
        in->canonical = canon;
}

// True when generation record "in" has a constructor key that the canonical record "canon" does
// not know (S27). Then the canonical "construct" cannot build the shell for that key. A new core
// module provides it.
bool has_new_ctor_key(const ClassRecord* canon, const ClassRecord* in)
{
    if (!in->construct_keys)
        return false;
    for (const char* const* k = in->construct_keys; *k; ++k)
    {
        bool known = false;
        if (canon->construct_keys)
            for (const char* const* c = canon->construct_keys; *c && !known; ++c)
                known = std::strcmp(*c, *k) == 0;
        if (!known)
            return true;
    }
    return false;
}

// Makes the core instance of "m" (module.h) and registers it in "reg". The core must register
// the same classes and thunks as "code", in other memory. On false there is no core and a
// warning tells why; then "m" keeps the parts. The function clears m.make_core in each case.
bool open_core(Module& m, const Registry& code, Module& core, Registry& reg)
{
    if (!m.make_core)
        return false;
    const std::function<LoadResult()> make = std::move(m.make_core);
    m.make_core = nullptr;
    std::string why;
    LoadResult r;
    try
    {
        r = make();
    }
    catch (...)
    {
        r.error = "make_core threw an exception";
    }
    if (!r || !r.module->register_fn)
    {
        why = r ? std::string("no script_register") : r.error;
    }
    else
    {
        try
        {
            r.module->register_fn(reg);
        }
        catch (...)
        {
            why = "script_register of the core threw an exception";
        }
    }
    if (why.empty())
    {
        bool same = reg.entries().size() == code.entries().size() && reg.thunks().size() == code.thunks().size();
        for (const auto& [name, e] : code.entries())
        {
            const ClassEntry* c = reg.find(name);
            same = same && c && c->record != e.record && c->record->generation == e.record->generation
                && c->record->shell_hash == e.record->shell_hash;
        }
        for (const auto& [key, t] : code.thunks())
        {
            auto it = reg.thunks().find(key);
            same = same && it != reg.thunks().end() && it->second.fn && it->second.fn != t.fn;
        }
        if (!same)
            why = "the core does not register the same classes and thunks in its own memory";
    }
    if (!why.empty())
    {
        std::fprintf(stderr, "[script] WARNING: no core module for %s (%s); the generation stays loaded\n",
            m.origin.c_str(), why.c_str());
        return false;
    }
    core = std::move(*r.module);
    return true;
}
} // namespace

namespace
{
std::atomic<RestartHandler> g_restart_handler{nullptr};
std::atomic<bool> g_restart_requested{false};
} // namespace

void set_restart_handler(RestartHandler h)
{
    g_restart_handler.store(h);
}
bool restart_requested()
{
    return g_restart_requested.load();
}
void clear_restart_request()
{
    g_restart_requested.store(false);
}

void request_restart(const char* reason)
{
    g_restart_requested.store(true);
    std::fprintf(stderr, "[script] FATAL: restart requested: %s\n", reason);
    if (RestartHandler h = g_restart_handler.load())
    {
        h(reason);
        return;
    }
    std::abort(); // no engine handler: the process cannot go on
}

Loader::Loader(bool host_thread_safe)
{
    check_thread_safe_build(host_thread_safe);
}

void check_thread_safe_build(bool host_thread_safe)
{
    if (host_thread_safe != (HOT_THREAD_SAFE != 0))
        throw std::runtime_error(std::string("HOT_THREAD_SAFE mismatch: the host uses ") + (host_thread_safe ? "1" : "0")
            + " but the runtime library uses " + (HOT_THREAD_SAFE ? "1" : "0")
            + ". Build the host and the runtime with the same HOT_THREAD_SAFE option.");
}

// The canonical record of an AOT class is the constant data of the program, and adopt() changes
// it. The loader puts the saved copy back, so a later loader in the same process starts from
// the generation 1 record.
void Loader::restore_aot_records()
{
    for (const auto& [name, saved] : aot_records_)
        if (const ClassEntry* e = registry_.find(name))
            *e->record = saved;
}

namespace
{
// The loader that the resolvers of the cross-module access use (install_resolvers).
std::atomic<Loader*> g_link_loader{nullptr};
} // namespace

Loader::~Loader()
{
    set_sync_mode(SyncMode::StopTheWorld); // the mode is global; the next loader starts clean
    Loader* self = this;
    if (g_link_loader.compare_exchange_strong(self, nullptr))
    { // the records and the tables of this loader go now
        detail::set_class_resolver(nullptr);
        detail::set_unit_resolver(nullptr);
    }
    restore_aot_records();
    for (auto& [unit, mb] : mod_blocks_)
    { // the code of the field destroyers is still loaded: "gens_" goes later
        destroy_fields(mb.layout, mb.block);
        aligned_free(mb.block);
    }
}

bool Loader::reset(std::string& error)
{
    // The checks change nothing. The first check that fails gives the text.
    if (entry_depth() != 0 || in_script_tick() != 0)
    {
        error = "reset was called inside script code";
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (aot_optimized_)
    {
        error = "the loader holds optimized AOT code (MODE AOT OPTIMIZED): objects are not tracked, "
                "so it cannot reset. Exit the program instead";
        return false;
    }
    if (pending_)
    {
        error = "a reload is pending";
        return false;
    }
    {
        const LiveObjects::Guard guard(g_live); // other threads can create objects now
        if (!g_live.empty())
        {
            std::map<std::string, size_t> by_name; // sorted, so the text is stable
            for (const auto& [s, creator] : g_live)
                ++by_name[s->cls->name];
            error = std::to_string(g_live.size()) + " live objects remain: ";
            bool first = true;
            for (const auto& [name, count] : by_name)
            {
                if (!first)
                    error += ", ";
                first = false;
                error += name + " x" + std::to_string(count);
            }
            return false;
        }
    }
    for (const Gen& g : gens_)
    {
        if (g.state != GenState::Unloaded && pin_count(g.generation) != 0)
        {
            error = "generation " + std::to_string(g.generation) + " is pinned (a closure or a task is alive)";
            return false;
        }
    }
    // The code of the field destroyers is still loaded here: "gens_" goes later.
    for (auto& [unit, mb] : mod_blocks_)
    {
        if (mb.slot)
            *mb.slot = nullptr;
        destroy_fields(mb.layout, mb.block);
        aligned_free(mb.block);
    }
    mod_blocks_.clear();
    restore_aot_records();
    unit_of_.clear();
    aot_records_.clear();
    aot_slots_.clear();
    aot_optimized_ = false;
    aot_units_.clear();
    revert_records_.clear();
    revert_slots_.clear();
    unit_live_.clear(); // the flags are in the code that unloads next
    removed_.clear();
    own_slots_.clear();
    linked_slots_.clear();
    slot_keys_.clear();
    eff_keys_.clear();
    iface_slots_.clear(); // the records go with "registry_" below
    iface_names_.clear();
    record_gen_.clear();
    links_.clear();
    old_fns_.clear();
    if (g_link_loader.load() == this)
        detail::set_class_resolver(&resolve_class_link); // clears the cache of class_ref
    ++link_epoch;
    registry_ = Registry{}; // the records point into the code that unloads next
    gens_.clear(); // unloads the libraries of the generations
    cores_.clear(); // after "gens_", as in the destructor
    key_text_.clear();
    frame_ = 0;
    active_generation = 0;
    clear_restart_request();
    return true;
}

bool Loader::make_module_block(const std::string& unit, const ModuleStateInfo& info, std::string& error)
{
    char* block = alloc_state(info.layout);
    if (!block)
    {
        error = "out of memory for the module state";
        return false;
    }
    *info.slot = block; // an initializer can read the block
    try
    {
        for (uint32_t i = 0; i < info.layout->field_count; ++i)
        {
            const FieldInfo& f = info.layout->fields[i];
            if (f.init)
                f.init(block, block + f.offset);
        }
    }
    catch (...)
    {
        *info.slot = nullptr;
        aligned_free(block); // the partial state is unknown: no destructor call
        error = "an initializer of a module variable threw an exception";
        return false;
    }
    mod_blocks_[unit] = {info.layout, info.slot, block};
    return true;
}

namespace
{

ReloadResult make_result(
    ReloadStatus status, ReloadReason reason, std::string error = {}, std::vector<std::string> classes = {})
{
    ReloadResult r;
    r.status = status;
    r.reason = reason;
    r.error = std::move(error);
    r.classes = std::move(classes);
    return r;
}

ReloadResult failed(ReloadReason reason, std::string error, std::vector<std::string> classes = {})
{
    return make_result(ReloadStatus::Failed, reason, std::move(error), std::move(classes));
}

ReloadResult full_reload(ReloadReason reason, std::string error, std::string cls)
{
    return make_result(ReloadStatus::FullReloadRequired, reason, std::move(error), {std::move(cls)});
}

const char* const kOptimizedLocked =
    "the loader holds optimized AOT code (MODE AOT OPTIMIZED): it cannot override or reload a script. "
    "Build the scripts without OPTIMIZED";

} // namespace

ReloadResult Loader::load(Module module)
{
    if (!module.register_fn)
        return failed(ReloadReason::BadModule, "the module has no script_register function");
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!gens_.empty()) // "active_generation" is global: a destroyed loader can leave it set
            return failed(ReloadReason::BadModule, "the loader already has a generation: call reset first");
    }
    if (ReloadResult r = load_unit(std::move(module)); !r)
        return r;
    return link_first_load();
}

// A script parent in another unit (modules step 5): link "super", check the parent layout and
// fill the inherited slots, after all the units of the first load are in the registry.
ReloadResult Loader::link_first_load()
{
    for (auto& [name, e] : registry_.entries())
    {
        ClassRecord* r = e.record;
        if (!r->super_name)
            continue;
        ClassEntry* pe = registry_.find(r->super_name);
        std::string why;
        if (!pe)
            why = "the script parent " + std::string(r->super_name) + " is not loaded";
        else if (layout_hash(pe->record->layout) != r->super_layout_hash)
            why = "the state layout of the script parent " + std::string(r->super_name)
                + " is not the layout that the class compiled against";
        if (!why.empty())
        {
            const std::string cls = name;
            std::string ignored;
            reset(ignored);
            ReloadResult f = failed(ReloadReason::ImportMismatch, "class " + cls + ": " + why);
            f.classes = {cls};
            return f;
        }
        r->super = pe->record;
        own_slots_[name] = r->slots;
    }
    link_inherited_slots();
    return make_result(ReloadStatus::Applied, ReloadReason::None);
}

ReloadResult Loader::load(std::vector<Module> modules)
{
    if (modules.empty())
        return failed(ReloadReason::BadModule, "the batch has no module");
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!gens_.empty())
            return failed(ReloadReason::BadModule, "the loader already has a generation: call reset first");
    }
    std::map<std::string, size_t> by_unit;
    for (size_t i = 0; i < modules.size(); ++i)
        if (!by_unit.emplace(modules[i].unit, i).second)
            return failed(ReloadReason::BadModule, "the batch has two modules of unit '" + modules[i].unit + "'");
    for (const Module& m : modules)
        for (const auto& [unit, hash] : m.imports)
        {
            const auto it = by_unit.find(unit);
            if (it == by_unit.end() || modules[it->second].iface_hash != hash)
            {
                ReloadResult r = failed(ReloadReason::ImportMismatch,
                    "unit '" + m.unit + "' imports '" + unit + "', which "
                        + (it == by_unit.end() ? "is not in the batch" : "has another interface hash"));
                r.units = {m.unit};
                return r;
            }
        }
    // An imported unit loads before its importer. A cycle keeps the order of the batch.
    std::vector<size_t> order;
    std::vector<int> mark(modules.size(), 0); // 0: new, 1: on the path, 2: done
    const std::function<void(size_t)> visit = [&](size_t i)
    {
        if (mark[i] != 0)
            return;
        mark[i] = 1;
        for (const auto& [unit, hash] : modules[i].imports)
            visit(by_unit.at(unit));
        mark[i] = 2;
        order.push_back(i);
    };
    for (size_t i = 0; i < modules.size(); ++i)
        visit(i);
    for (size_t i : order)
    {
        const std::string unit = modules[i].unit;
        ReloadResult r = load_unit(std::move(modules[i]));
        if (!r)
        {
            std::string ignored;
            reset(ignored); // no object exists yet: the loader is empty again
            r.error = "unit '" + unit + "': " + r.error;
            return r;
        }
    }
    return link_first_load();
}

void Loader::install_resolvers()
{
    g_link_loader.store(this);
    detail::set_class_resolver(&resolve_class_link); // clears the cache of class_ref
    detail::set_unit_resolver(&resolve_unit_link);
}

void Loader::set_link(const Module& module, const Registry& code)
{
    UnitLink& link = links_[module.unit];
    link.iface_hash = module.iface_hash;
    link.imports = module.imports;
    if (link.fns)
        old_fns_.push_back(std::move(link.fns));
    link.fns = std::make_unique<FnTable>(); // a new id
    link.fns->entries = code.fn_table().entries;
}

ClassRecord* Loader::resolve_class_link(const char* name)
{
    const Loader* l = g_link_loader.load();
    const ClassEntry* e = l ? l->registry_.find(name) : nullptr;
    return e ? e->record : nullptr;
}

bool Loader::resolve_unit_link(const char* name, UnitInfo& out)
{
    const Loader* l = g_link_loader.load();
    if (!l)
        return false;
    const auto it = l->links_.find(name);
    if (it == l->links_.end())
        return false;
    out.fns = it->second.fns.get();
    if (const auto mb = l->mod_blocks_.find(name); mb != l->mod_blocks_.end())
    {
        out.layout = mb->second.layout;
        out.block = mb->second.block;
    }
    return true;
}

ReloadResult Loader::load_aot(const AotTable& table)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!gens_.empty())
            return failed(ReloadReason::BadModule, "the loader already has a generation: call reset first");
    }
    if (table.count == 0)
        return failed(ReloadReason::BadModule, "the AOT table has no script");
    for (size_t i = 0; i < table.count; ++i)
    {
        Module m;
        m.register_fn = table.scripts[i].register_fn;
        m.origin = std::string("aot:") + table.scripts[i].name;
        m.unit = table.scripts[i].name;
        Registry code;
        ReloadResult r = load_unit(std::move(m), &code);
        if (r)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            AotUnit& au = aot_units_[table.scripts[i].name];
            au.register_fn = table.scripts[i].register_fn;
            au.reg = std::move(code);
        }
        else
        {
            std::string ignored;
            reset(ignored); // no object exists yet: the loader is empty again
            r.error = std::string("AOT script ") + table.scripts[i].name + ": " + r.error;
            return r;
        }
    }
    std::lock_guard<std::mutex> lock(mutex_);
    aot_slots_ = table.slots ? table.slots : "";
    aot_optimized_ = table.optimized; // from now on the loader refuses an override and a reload
    for (const auto& [name, e] : registry_.entries())
    {
        // The saved copy has no key table: the table belongs to the loader and can move or go.
        // The commit of a revert sets the key table of the canonical record again.
        ClassRecord& saved = aot_records_.emplace(name, *e.record).first->second;
        saved.slot_keys = nullptr;
        saved.iface_slots = nullptr; // the same: adopt_ifaces rebuilds the table at the revert
        saved.iface_slot_count = 0;
    }
    return make_result(ReloadStatus::Applied, ReloadReason::None);
}

ReloadResult Loader::load_unit(Module module, Registry* code_out)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (aot_optimized_)
            return failed(ReloadReason::BadModule, kOptimizedLocked);
    }
    if (!module.register_fn)
        return failed(ReloadReason::BadModule, "the module has no script_register function");
    Registry code;
    module.register_fn(code);
    // The core instance gives the canonical records, the factories and the thunks (module.h).
    Module core;
    Registry core_reg;
    const bool have_core = open_core(module, code, core, core_reg);
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [name, e] : code.entries())
        if (const auto u = unit_of_.find(name); u != unit_of_.end())
            return failed(ReloadReason::BadModule,
                "class " + name + " is in two units ('" + u->second + "' and '" + module.unit + "')", {name});
    if (links_.count(module.unit))
        return failed(ReloadReason::BadModule, "unit '" + module.unit + "' is loaded already");
    // An initializer of a module variable can use another unit: the links come first.
    install_resolvers();
    set_link(module, code);
    if (const ModuleStateInfo& ms = code.module_state(); ms.layout)
    {
        std::string error;
        if (mod_blocks_.count(module.unit))
            error = "the loader has a module state block already";
        else
            make_module_block(module.unit, ms, error);
        if (!error.empty())
        {
            links_.erase(module.unit);
            return failed(ReloadReason::BadModule, std::move(error));
        }
    }
    const Registry& canon = have_core ? core_reg : code;
    for (const auto& [name, e] : canon.entries())
    {
        registry_.add(e.record, e.create);
        unit_of_.emplace(name, module.unit);
        if (!have_core)
            adopt_ifaces(e.record, e.record);
    }
    for (const auto& [key, t] : canon.thunks())
    {
        registry_.add_thunk(t);
        if (!t.sig.empty())
            set_slot_key(t);
    }
    if (have_core)
        for (const auto& [name, e] : code.entries())
        {
            ClassRecord* ar = registry_.find(name)->record;
            adopt(ar, e.record);
            adopt_ifaces(ar, e.record);
        }
    refresh_slot_keys();
    const uint32_t gen = registry_.max_generation();
    if (!have_core)
        note_records(code, gen);
    std::function<void()> on_accept = module.on_accept;
    switch_live(module.unit, code, have_core ? &core_reg : nullptr);
    add_generation(std::move(module), gen);
    gens_.back().registry_refs = have_core ? 0u : static_cast<uint32_t>(code.entries().size() + code.thunks().size());
    if (have_core)
        cores_.push_back(std::move(core));
    active_generation = gen;
    // The module is accepted. The callback runs under the loader mutex: it must not call the Loader.
    if (on_accept)
        on_accept();
    if (code_out)
        *code_out = code;
    return make_result(ReloadStatus::Applied, ReloadReason::None);
}

void Loader::switch_live(const std::string& unit, const Registry& code, const Registry* core)
{
    std::vector<uint32_t*>& flags = unit_live_[unit];
    for (uint32_t* f : flags) // the older module is retired, not unloaded: the memory is valid
        *f = 0;
    flags.clear();
    for (const Registry* r : {&code, core})
        if (r && r->live_flag())
        {
            *r->live_flag() = 1;
            flags.push_back(r->live_flag());
        }
}

// Remembers which module holds each record of a registry.
void Loader::note_records(const Registry& reg, uint32_t generation)
{
    for (const auto& [name, e] : reg.entries())
        record_gen_.emplace(e.record, generation);
}

void Loader::add_generation(Module module, uint32_t generation)
{
    Gen g;
    g.generation = generation;
    g.module = std::move(module);
    gens_.push_back(std::move(g));
}

// An object uses the code of up to three generations:
//   - the module that holds its record, when the record is not in a core (record_gen_);
//   - the generation of the record: the layout, the field functions and the slots. For a
//     removed class this is the last generation that had the class;
//   - the generation whose "new" made the object and its handle (g_live).
// The vtable is in the module that holds the record.
std::map<Loader::ModuleKey, uint32_t> Loader::live_by_module() const
{
    // The unit of a generation number: the unit of its module when one module has the number.
    // The AOT units share generation 1; then the unit of the class of the object decides.
    std::unordered_map<uint32_t, std::pair<int, const std::string*>> owner;
    for (const Gen& g : gens_)
    {
        auto& o = owner.try_emplace(g.generation, 0, nullptr).first->second;
        ++o.first;
        o.second = &g.module.unit;
    }
    static const std::string kNoUnit;
    std::map<ModuleKey, uint32_t> live;
    const LiveObjects::Guard guard(g_live);
    for (const auto& [s, creator] : g_live)
    {
        const auto cu = unit_of_.find(s->cls->name);
        const std::string& class_unit = cu == unit_of_.end() ? kNoUnit : cu->second;
        auto unit_for = [&](uint32_t gen) -> const std::string&
        {
            const auto o = owner.find(gen);
            return o != owner.end() && o->second.first == 1 && o->second.second ? *o->second.second : class_unit;
        };
        uint32_t used[3] = {s->cls->generation, creator, 0};
        if (auto it = record_gen_.find(s->cls); it != record_gen_.end())
            used[2] = it->second;
        for (int i = 0; i < 3; ++i)
        {
            const bool seen = (i > 0 && used[i] == used[0]) || (i > 1 && used[i] == used[1]);
            if (used[i] != 0 && !seen)
                ++live[{unit_for(used[i]), used[i]}];
        }
    }
    return live;
}

namespace
{
// True when a field of the layout, or of a struct in it, has the flag kMayHoldCode.
bool layout_holds_code(const StateLayout* l, int depth = 0)
{
    if (!l || depth > 32)
        return false;
    for (uint32_t i = 0; i < l->field_count; ++i)
    {
        const FieldInfo& f = l->fields[i];
        if (f.flags & kMayHoldCode)
            return true;
        if ((f.flags & kScriptStruct) && layout_holds_code(f.sub, depth + 1))
            return true;
    }
    return false;
}
} // namespace

void Loader::each_object(const std::function<void(ShellBase*)>& visit) const
{
    const LiveObjects::Guard guard(g_live);
    for (const auto& [s, creator] : g_live)
        visit(s);
}

bool Loader::state_holds_code() const
{
    for (const auto& [unit, mb] : mod_blocks_)
        if (layout_holds_code(mb.layout))
            return true;
    std::unordered_map<const StateLayout*, bool> seen;
    const LiveObjects::Guard guard(g_live);
    for (const auto& [s, creator] : g_live)
    {
        auto [it, fresh] = seen.try_emplace(s->cls->layout, false);
        if (fresh)
            it->second = layout_holds_code(s->cls->layout);
        if (it->second)
            return true;
    }
    return false;
}

uint32_t Loader::code_refs(const Gen& g, const std::map<ModuleKey, uint32_t>& live) const
{
    auto it = live.find({g.module.unit, g.generation});
    return g.registry_refs + (it == live.end() ? 0 : it->second);
}

unsigned Loader::frame_boundary()
{
    unsigned unloaded = 0;
    std::map<ModuleKey, uint32_t> live;
    bool have_live = false;
    for (Gen& g : gens_)
    {
        if (g.state != GenState::Retired || g.retired_frame >= frame_)
            continue;
        if (pin_count(g.generation) != 0)
            continue;
        if (!have_live)
        {
            // A field value can point to the code of any generation that ran (design 4.9).
            if (state_holds_code())
                break;
            live = live_by_module(), have_live = true;
        }
        if (code_refs(g, live) != 0)
            continue;
        // A removed class without objects can still have its last layout in this generation.
        // A new object of it (the factory) gets an empty state then.
        for (const auto& [name, table] : removed_)
            if (ClassEntry* e = registry_.find(name); e && e->record->generation == g.generation)
                e->record->layout = &kEmptyLayout;
        // Only the records of this module go: another unit can have the same generation number.
        for (auto it = record_gen_.begin(); it != record_gen_.end();)
        {
            const auto cu = unit_of_.find(it->first->name);
            const bool mine = it->second == g.generation && cu != unit_of_.end() && cu->second == g.module.unit;
            it = mine ? record_gen_.erase(it) : std::next(it);
        }
        g.module = Module{}; // the keep-alive object releases the code
        g.state = GenState::Unloaded;
        std::fprintf(stderr, "[script] generation %u unloaded\n", g.generation);
        ++unloaded;
    }
    ++frame_;
    if (unloaded != 0)
        ++link_epoch; // a new layout can get the address of an unloaded layout
    return unloaded;
}

LoaderStats Loader::stats() const
{
    LoaderStats st;
    st.active_generation = active_generation;
    {
        const LiveObjects::Guard guard(g_live);
        st.live_objects = g_live.size();
    }
    st.core_modules = cores_.size();
    st.state_holds_code = state_holds_code();
    const auto live = live_by_module();
    for (const Gen& g : gens_)
        st.generations.push_back({g.generation, g.state, pin_count(g.generation), code_refs(g, live), g.module.origin});
    std::unordered_map<const ClassRecord*, size_t> by_class;
    {
        const LiveObjects::Guard guard(g_live);
        for (const auto& [s, creator] : g_live)
            ++by_class[s->cls];
    }
    for (const auto& [name, e] : registry_.entries())
    {
        const auto it = by_class.find(e.record);
        std::vector<std::string> attributes;
        for (const char* const* a = e.record->attributes; a && *a; ++a)
            attributes.emplace_back(*a);
        st.classes.push_back({name, e.record->generation, e.record->slot_count, it == by_class.end() ? 0 : it->second,
            removed_.count(name) != 0, std::move(attributes)});
        st.slots += e.record->slot_count;
    }
    std::sort(
        st.classes.begin(), st.classes.end(), [](const ClassInfo& a, const ClassInfo& b) { return a.name < b.name; });
    st.removed_class_calls = removed_class_call_count();
    return st;
}

std::string format_stats(const LoaderStats& st)
{
    static const char* const kState[] = {"active", "retired", "unloaded"};
    std::string o = "script_stats: active generation " + std::to_string(st.active_generation) + ", "
        + std::to_string(st.live_objects) + " live objects, " + std::to_string(st.classes.size()) + " classes, "
        + std::to_string(st.slots) + " slots\n";
    for (const GenerationInfo& g : st.generations)
        o += "  generation " + std::to_string(g.generation) + ": " + kState[static_cast<int>(g.state)] + ", pins "
            + std::to_string(g.pins) + ", code refs " + std::to_string(g.code_refs) + ", " + g.origin + "\n";
    for (const ClassInfo& c : st.classes)
        o += "  class " + c.name + ": generation " + std::to_string(c.generation) + ", " + std::to_string(c.slots)
            + " slots, " + std::to_string(c.live_objects) + " live" + (c.removed ? ", REMOVED" : "") + "\n";
    o += "  core modules: " + std::to_string(st.core_modules) + "\n";
    if (st.state_holds_code)
        o += "  unload blocked: a live state field can point to the code of a generation\n";
    o += "  removed class calls ignored: " + std::to_string(st.removed_class_calls) + "\n";
    return o;
}

namespace
{
} // namespace

void Loader::set_slot_key(const ThunkEntry& t)
{
    const std::string cls = t.name.substr(0, t.name.find("::"));
    ClassEntry* ce = registry_.find(cls);
    if (!ce)
        return;
    ClassRecord* r = ce->record;
    std::vector<const char*>& keys = slot_keys_[cls];
    const size_t n = std::max<size_t>(r->slot_count, t.slot + 1);
    if (keys.size() < n)
        keys.resize(n, nullptr);
    keys[t.slot] = key_text_.insert(t.sig).first->c_str(); // the module of "t" can unload
}

// The key table of a class has the key of each keyed thunk. A slot that holds the removed handler
// is defunct, and a call through it is wrong for a result that the caller builds in a buffer
// (std::string, a handle, a struct), so such a slot gets a key that no signature matches. A slot of
// a removed class gets kRemovedSlotKey: hot::call detaches and makes a default result. A defunct
// slot of a live class (a removed function) gets kDefunctSlotKey: the call throws ScriptError. A
// defunct slot that a function reuses gets its own key back at the next refresh. The tables stay allocated, so a record keeps a valid pointer.
void Loader::refresh_slot_keys()
{
    const SlotFn removed = reinterpret_cast<SlotFn>(&StateBase::__removed);
    for (const auto& [name, e] : registry_.entries())
    {
        ClassRecord* r = e.record;
        const auto real = slot_keys_.find(name);
        const auto eff = eff_keys_.find(name);
        bool defunct = false;
        for (uint32_t i = 0; r->slots && i < r->slot_count; ++i)
            defunct |= r->slots[i] == removed;
        if (!defunct && real == slot_keys_.end() && eff == eff_keys_.end())
            continue;
        std::vector<const char*>& keys = eff_keys_[name];
        keys.assign(r->slot_count, nullptr);
        if (real != slot_keys_.end())
            for (size_t i = 0; i < keys.size() && i < real->second.size(); ++i)
                keys[i] = real->second[i];
        const char* const defunct_key = removed_.count(name) ? kRemovedSlotKey : kDefunctSlotKey;
        for (uint32_t i = 0; r->slots && i < r->slot_count; ++i)
            if (r->slots[i] == removed)
                keys[i] = defunct_key;
        r->slot_keys = keys.data();
    }
}

const char* const* Loader::session_ifaces(const char* const* list)
{
    if (!list)
        return nullptr;
    std::vector<std::string> names;
    for (const char* const* n = list; *n; ++n)
        names.emplace_back(*n);
    auto it = iface_names_.find(names);
    if (it == iface_names_.end())
    {
        it = iface_names_.emplace(std::move(names), std::vector<const char*>()).first;
        std::vector<const char*> out;
        for (const std::string& s : it->first)
            out.push_back(s.c_str());
        out.push_back(nullptr);
        it->second = std::move(out);
    }
    return it->second.data();
}

void Loader::adopt_ifaces(ClassRecord* canon, const ClassRecord* in)
{
    // The interface names. A canonical record that is not "in" gets a list that lives for the
    // session. When "canon" is "in", the list stays in the module of the record, which lives as long as
    // the record. The saved record of an AOT class (aot_records_) needs that list for a revert.
    if (canon != in)
        canon->ifaces = session_ifaces(in->ifaces);
    // The table from the id to the slot. The ids are global, so the table covers the highest id
    // of this class only. A missing id is "not implemented".
    std::vector<uint32_t> table = build_iface_table(in->iface_rows);
    if (table.empty())
    {
        iface_slots_.erase(canon->name);
        canon->iface_slots = nullptr;
        canon->iface_slot_count = 0;
    }
    else
    {
        std::vector<uint32_t>& owned = iface_slots_[canon->name];
        owned = std::move(table);
        canon->iface_slots = owned.data();
        canon->iface_slot_count = static_cast<uint32_t>(owned.size());
    }
    // The rows belong to the module of "in". The canonical record keeps them only when it is "in".
    if (canon != in)
        canon->iface_rows = nullptr;
}

void* removed_class_handler()
{
    note_removed_call();
    return nullptr;
}

HOT_EXPORT void* StateBase::__removed()
{
    return removed_class_handler();
}

uint64_t removed_class_call_count()
{
    return removed_call_count();
}

ReloadResult Loader::request_reload(LoadResult built)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (aot_optimized_)
            return failed(ReloadReason::BadModule, kOptimizedLocked);
    }
    // The host checked the LoadResult of the backend itself, then it calls this overload. A
    // backend failure becomes a result here. So a host needs no own mapping of the reason: a
    // SlotsRemoved failure is a FullReloadRequired result. It is a safety net: hotc does not give it.
    if (!built.module)
    {
        const ReloadReason reason = built.reason == ReloadReason::None ? ReloadReason::CompileError : built.reason;
        if (reason == ReloadReason::SlotsRemoved)
            return make_result(ReloadStatus::FullReloadRequired, reason, std::move(built.error));
        return failed(reason, std::move(built.error));
    }
    return request_reload(std::move(*built.module));
}

ReloadResult Loader::request_reload(std::vector<LoadResult> built)
{
    std::vector<Module> modules;
    modules.reserve(built.size());
    for (LoadResult& r : built)
    {
        if (!r.module)
            return request_reload(std::move(r)); // the mapping of a backend failure
        modules.push_back(std::move(*r.module));
    }
    return request_reload(std::move(modules));
}

ReloadResult Loader::request_reload(Module module)
{
    std::vector<Module> modules;
    modules.push_back(std::move(module));
    return request_reload(std::move(modules));
}

ReloadResult Loader::request_reload(std::vector<Module> modules)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (aot_optimized_)
            return failed(ReloadReason::BadModule, kOptimizedLocked);
        if (pending_)
            return failed(ReloadReason::Busy, "a reload is already pending");
    }
    if (modules.empty())
        return failed(ReloadReason::BadModule, "the batch has no module");
    auto p = std::make_unique<Pending>();
    p->units.resize(modules.size());
    for (size_t i = 0; i < modules.size(); ++i)
    {
        PendingUnit& u = p->units[i];
        if (!modules[i].register_fn)
            return failed(ReloadReason::BadModule, "the module has no script_register function");
        u.module = std::move(modules[i]);
        try
        {
            u.module.register_fn(u.staged);
        }
        catch (...)
        {
            return failed(ReloadReason::BadModule, "script_register threw an exception");
        }
    }
    for (PendingUnit& u : p->units)
    {
        // A generation that adds a class or a thunk gets a core instance (module.h). Only a commit
        // changes "registry_", and a commit needs "pending_". The check of "pending_" in
        // validate_and_stage rejects this request when another request was faster.
        bool adds = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const auto& [name, e] : u.staged.entries())
            {
                const ClassEntry* ce = registry_.find(name);
                adds = adds || !ce || has_new_ctor_key(ce->record, e.record);
            }
            for (const auto& [key, t] : u.staged.thunks())
                adds = adds || !registry_.has_thunk(key);
        }
        if (adds)
            u.have_core = open_core(u.module, u.staged, u.core, u.core_reg);
        u.module.make_core = nullptr; // the JIT keeps a copy of the IR for it
    }
    return validate_and_stage(std::move(p));
}

namespace
{
// The lines of the slot manifest "aot" for the classes in "classes". A line is "Cls::fn gen" or
// "sig Cls::fn Sig".
std::string slot_lines_of(const std::string& aot, const std::set<std::string>& classes)
{
    std::string out;
    for (size_t pos = 0; pos < aot.size();)
    {
        size_t eol = aot.find('\n', pos);
        if (eol == std::string::npos)
            eol = aot.size();
        const std::string line = aot.substr(pos, eol - pos);
        pos = eol + 1;
        const size_t b = line.rfind("sig ", 0) == 0 ? 4 : 0;
        const size_t sp = line.find(' ', b);
        const std::string fn = line.substr(b, sp == std::string::npos ? std::string::npos : sp - b);
        const size_t e = fn.rfind("::");
        if (e != std::string::npos && classes.count(fn.substr(0, e)))
            out += line + "\n";
    }
    return out;
}
} // namespace

ReloadResult Loader::revert_unit(const std::string& unit, Backend& backend)
{
    auto p = std::make_unique<Pending>();
    p->units.resize(1);
    PendingUnit& pu = p->units[0];
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (aot_optimized_)
            return failed(ReloadReason::BadModule, kOptimizedLocked);
        if (pending_)
            return failed(ReloadReason::Busy, "a reload is already pending");
        const auto au = aot_units_.find(unit);
        if (au == aot_units_.end())
            return failed(ReloadReason::BadModule, "unit '" + unit + "' has no AOT module to revert to");
        pu.module.register_fn = au->second.register_fn;
        pu.module.origin = "aot:" + unit + " (revert)";
        pu.module.unit = unit;
        std::set<std::string> classes;
        for (const auto& [name, e] : au->second.reg.entries())
            classes.insert(name);
        // The commit runs this callback. The backend gets the AOT signatures of this unit only.
        pu.module.on_accept = [&backend, lines = slot_lines_of(aot_slots_, classes)]
        { backend.revert_slots(lines); };
        // A fresh generation number: the "generation is newer" check of the validation passes.
        const uint32_t generation = active_generation + 1;
        Registry& st = pu.staged;
        for (const auto& [fn_name, fe] : au->second.reg.fn_table().entries)
            st.add_fn(fn_name, fe.sig.c_str(), fe.fn);
        // The AOT thunks make the AOT signature the current one again. The commit sets the slot
        // keys of the AOT signatures again (see commit_pending).
        for (const auto& [key, t] : au->second.reg.thunks())
            st.add_thunk(t);
        st.set_live_flag(au->second.reg.live_flag());
        if (const ModuleStateInfo& ms = au->second.reg.module_state(); ms.layout)
            st.set_module_state(ms.layout, ms.slot);
        for (const auto& [name, e] : au->second.reg.entries())
        {
            const ClassEntry* cur = registry_.find(name);
            const auto saved = aot_records_.find(name);
            if (!cur || saved == aot_records_.end())
                return failed(ReloadReason::BadModule, "class " + name + " of the AOT unit is not registered", {name});
            ClassRecord copy = saved->second; // the AOT record as load_aot saw it
            copy.generation = generation;
            // The dispatch table must not shrink (a reload with fewer slots needs a full reload).
            // The slots that an override added point to the removed class handler.
            if (cur->record->slot_count > copy.slot_count)
            {
                std::vector<SlotFn> table(copy.slots, copy.slots + copy.slot_count);
                table.resize(cur->record->slot_count, reinterpret_cast<SlotFn>(&StateBase::__removed));
                revert_slots_.push_back(std::move(table));
                copy.slots = revert_slots_.back().data();
                copy.slot_count = static_cast<uint32_t>(revert_slots_.back().size());
                // The name array has only the entries of the AOT record. The padded slots have no name.
                copy.slot_names = nullptr;
            }
            revert_records_.push_back(copy);
            st.add(&revert_records_.back(), e.create);
        }
    }
    return validate_and_stage(std::move(p));
}

ReloadResult Loader::check_links(const Pending& p) const
{
    std::map<std::string, const Module*> batch;
    std::map<std::string, std::string> owner; // a class of the batch to its unit
    for (const PendingUnit& u : p.units)
    {
        if (!batch.emplace(u.module.unit, &u.module).second)
            return failed(ReloadReason::BadModule, "the batch has two modules of unit '" + u.module.unit + "'");
        for (const auto& [name, e] : u.staged.entries())
            if (const auto [it, added] = owner.emplace(name, u.module.unit); !added)
                return failed(ReloadReason::BadModule,
                    "class " + name + " is in two units ('" + it->second + "' and '" + u.module.unit + "')", {name});
    }
    // An import in the batch or live must have the interface hash that the importer compiled against.
    // An import that is not a unit is bridged: the importer has a copy of it (the load accepts it too).
    for (const PendingUnit& u : p.units)
        for (const auto& [unit, hash] : u.module.imports)
        {
            std::string why;
            if (const auto b = batch.find(unit); b != batch.end())
            {
                if (b->second->iface_hash != hash)
                    why = "the batch module of '" + unit + "' has another interface hash";
            }
            else if (const auto l = links_.find(unit); l == links_.end())
                continue;
            else if (l->second.iface_hash != hash)
                why = "the live module of '" + unit + "' has another interface hash; add it to the batch";
            if (!why.empty())
            {
                ReloadResult r = failed(ReloadReason::ImportMismatch, "unit '" + u.module.unit + "' imports '" + unit + "': " + why);
                r.units = {u.module.unit};
                return r;
            }
        }
    // A live unit outside the batch that imports a changed interface must be in the batch.
    std::vector<std::string> stale;
    for (const auto& [unit, link] : links_)
    {
        if (batch.count(unit))
            continue;
        for (const auto& [dep, hash] : link.imports)
            if (const auto b = batch.find(dep); b != batch.end() && b->second->iface_hash != hash)
            {
                stale.push_back(unit);
                break;
            }
    }
    if (!stale.empty())
    {
        std::sort(stale.begin(), stale.end());
        std::string list;
        for (const std::string& u : stale)
            list += (list.empty() ? "'" : ", '") + u + "'";
        ReloadResult r = failed(ReloadReason::DependentsStale,
            "the live units " + list + " import a changed interface of the batch; compile them into the batch");
        r.units = std::move(stale);
        return r;
    }
    return make_result(ReloadStatus::Pending, ReloadReason::None);
}

ReloadResult Loader::validate_and_stage(std::unique_ptr<Pending> p)
{
    // Validation. Nothing here changes a live class or a live object.
    // The lock keeps "registry_" and the class records stable. Only
    // commit_pending changes them, and it takes the same lock.
    std::lock_guard<std::mutex> lock(mutex_);
    if (pending_) // another thread was faster
        return failed(ReloadReason::Busy, "a reload is already pending");
    if (ReloadResult links = check_links(*p); !links)
        return links;
    for (PendingUnit& u : p->units)
    {
        // A class that is missing in the new generation is removed (design 6.1): no rejection.
        for (const auto& [name, in] : u.staged.entries())
        {
            ClassEntry* canon = registry_.find(name);
            if (!in.record->layout)
                return failed(ReloadReason::BadModule, "class " + name + " has no state layout", {name});
            if (in.record->generation <= active_generation)
                return failed(ReloadReason::GenerationNotNewer,
                    "class " + name + ": generation " + std::to_string(in.record->generation)
                        + " is not newer than the active generation " + std::to_string(active_generation),
                    {name});
            PendingClass pc{canon, in.record, in.create, {}};
            // A parent in another unit: its layout must be the layout that the child compiled against.
            if (in.record->super_name)
            {
                const ClassRecord* parent = nullptr;
                for (const PendingUnit& pu : p->units)
                    if (const ClassEntry* pe = pu.staged.find(in.record->super_name))
                        parent = pe->record;
                if (!parent)
                    if (const ClassEntry* pe = registry_.find(in.record->super_name); pe && !removed_.count(pe->record->name))
                        parent = pe->record;
                if (!parent)
                    return failed(ReloadReason::ImportMismatch,
                        "class " + name + ": the script parent " + in.record->super_name + " is not live and not in the batch",
                        {name});
                if (layout_hash(parent->layout) != in.record->super_layout_hash)
                    return failed(ReloadReason::ImportMismatch,
                        "class " + name + ": the state layout of the script parent " + in.record->super_name
                            + " is not the layout that the class compiled against",
                        {name});
            }
            if (canon)
            {
                if (const auto uo = unit_of_.find(name); uo != unit_of_.end() && uo->second != u.module.unit)
                    return failed(ReloadReason::BadModule,
                        "class " + name + " belongs to unit '" + uo->second + "' and cannot move to unit '"
                            + u.module.unit + "'",
                        {name});
                const ClassRecord* cur = canon->record;
                const char* b0 = cur->base_name ? cur->base_name : "";
                const char* b1 = in.record->base_name ? in.record->base_name : "";
                if (std::strcmp(b0, b1) != 0)
                {
                    // "base_name" lists the native bases: the primary base, then the interface bases
                    // (design 4.4). An added or removed interface base changes the shell (5.3: RECONSTRUCT).
                    const std::string s0 = b0, s1 = b1;
                    if (s0.substr(0, s0.find(',')) == s1.substr(0, s1.find(',')))
                        return full_reload(ReloadReason::InterfaceBasesChanged,
                            "class " + name + ": native interface bases changed (" + s0 + " -> " + s1
                                + "; RECONSTRUCT change)",
                            name);
                    return full_reload(ReloadReason::NativeBaseChanged,
                        "class " + name + ": base class changed (" + s0 + " -> " + s1 + ")", name);
                }
                // A change of the script parent changes the shell hierarchy (design 5.3: RECONSTRUCT).
                const char* p0 = parent_name(cur);
                const char* p1 = parent_name(in.record);
                if (std::strcmp(p0, p1) != 0)
                    return full_reload(ReloadReason::ScriptParentChanged,
                        "class " + name + ": script parent class changed (" + (*p0 ? p0 : "none") + " -> "
                            + (*p1 ? p1 : "none") + "; RECONSTRUCT change)",
                        name);
                if (cur->shell_hash != in.record->shell_hash)
                {
                    std::string error;
                    if (!cur->shares_this && in.record->shares_this)
                        error = "class " + name +
                                ": the new generation uses 'this' as a handle (or adds a script child) for the "
                                "first time. The shell then derives from std::enable_shared_from_this and its "
                                "layout changes (RECONSTRUCT change). Restart the program, or use 'this' as a "
                                "handle in generation 1";
                    else if (cur->shares_this && !in.record->shares_this)
                        error = "class " + name +
                                ": the shell of the new generation has no std::enable_shared_from_this, so its "
                                "layout changes (RECONSTRUCT change). Restart the program";
                    else
                        error = "class " + name + ": shell layout hash differs (RECONSTRUCT change)";
                    return full_reload(ReloadReason::ShellLayoutChanged, std::move(error), name);
                }
                if (in.record->slot_count < cur->slot_count)
                    return full_reload(
                        ReloadReason::SlotsRemoved, "class " + name + ": the new generation has fewer slots", name);
                // A type change does not reject the reload (design 4.7): the plan converts the
                // field or uses the default value. The commit logs the warnings of the plan.
                pc.plan = build_plan(*cur->layout, *in.record->layout);
            }
            u.classes.push_back(std::move(pc));
        }
        // The module state block migrates like the fields of a class (design 5.3). A type change
        // never rejects the reload. Without a block in the old generation, all variables are new.
        if (const ModuleStateInfo& nms = u.staged.module_state(); nms.layout)
        {
            if (!nms.slot)
                return failed(ReloadReason::BadModule, "the module state has no slot");
            const auto mb = mod_blocks_.find(u.module.unit);
            u.mod_plan = build_plan(mb != mod_blocks_.end() ? *mb->second.layout : kEmptyLayout, *nms.layout);
        }
    }
    // The modules are valid. The callback "on_accept" runs when the commit makes the generation live.
    pending_ = std::move(p);
    return make_result(ReloadStatus::Pending, ReloadReason::None);
}

void Loader::set_sync_mode(SyncMode mode)
{
    detail::store_entry_counters(kEntryCounters && mode == SyncMode::EntryCounters);
}

SyncMode Loader::sync_mode() const
{
    return entry_counters_enabled() ? SyncMode::EntryCounters : SyncMode::StopTheWorld;
}

ReloadResult Loader::commit_pending()
{
    if (entry_counters_enabled())
    {
        // EntryCounters mode (design 6.3). The thread must not wait for its own entry.
        if (entry_depth() != 0)
            return failed(
                ReloadReason::Busy, "commit_pending was called inside script code; the commit would wait for itself");
        if (!reload_pending())
            return failed(ReloadReason::None, {}); // no window when nothing is pending
        CommitWindow window; // no thread is in script code until the end of the scope
        std::lock_guard<std::mutex> lock(mutex_);
        return commit_locked();
    }
    // StopTheWorld mode. Debug check (design 6.3): all entry depths are zero. Count first,
    // then assert. In a release build kTickChecks is false and this code is gone.
    if constexpr (kTickChecks)
    {
        const int busy = in_script_tick();
        if (busy != 0)
        {
            g_unsafe_commits.fetch_add(1);
            std::fprintf(stderr, "[script] BUG: commit with %d thread(s) in script code\n", busy);
        }
        assert(busy == 0 && "commit_pending must run at the safe point");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    return commit_locked();
}

ReloadResult Loader::commit_locked()
{
    if (!pending_)
        return failed(ReloadReason::None, {});
    Pending& P = *pending_;

    struct Work
    {
        ShellBase* obj;
        const StateLayout* old_layout;
        char* old_block;
        char* new_block;
        const PendingClass* pc;
    };
    // The module block of one unit of the batch: the old block (if any) and the new block.
    struct BlockWork
    {
        PendingUnit* u;
        bool had_old = false;
        ModBlock old;
        char* new_mod = nullptr;
    };
    std::vector<Work> work;
    std::vector<BlockWork> blocks;
    for (PendingUnit& u : P.units)
    {
        BlockWork bw;
        bw.u = &u;
        if (const auto mb = mod_blocks_.find(u.module.unit); mb != mod_blocks_.end())
            bw.had_old = true, bw.old = mb->second;
        blocks.push_back(bw);
    }
    const auto free_new = [&]
    {
        for (Work& w : work)
            aligned_free(w.new_block);
        for (BlockWork& bw : blocks)
        {
            if (bw.new_mod && bw.u->staged.module_state().slot)
                *bw.u->staged.module_state().slot = nullptr;
            aligned_free(bw.new_mod);
            bw.new_mod = nullptr;
        }
    };

    // Step 1: allocate all new blocks. A failure here changes nothing.
    try
    {
        for (BlockWork& bw : blocks)
            if (const ModuleStateInfo& nms = bw.u->staged.module_state(); nms.layout)
            {
                bw.new_mod = alloc_state(nms.layout);
                if (!bw.new_mod)
                    throw std::bad_alloc();
            }
        // The pass holds the lock, but the migration after it does not (it runs script code). So
        // no thread creates or destroys an object while commit_pending runs (runtime_internal.h).
        const LiveObjects::Guard guard(g_live);
        work.reserve(g_live.size());
        for (const auto& [s, creator] : g_live)
        {
            const PendingClass* pc = nullptr;
            for (const PendingUnit& u : P.units)
                for (const PendingClass& c : u.classes)
                    if (c.canon && c.canon->record == s->cls)
                        pc = &c;
            if (!pc)
                continue;
            char* b = alloc_state(pc->incoming->layout);
            if (!b)
                throw std::bad_alloc();
            set_owner(pc->incoming->layout, b, s);
            work.push_back({s, s->cls->layout, static_cast<char*>(s->state), b, pc});
        }
    }
    catch (...)
    {
        free_new();
        pending_.reset();
        return failed(ReloadReason::MigrationFailed, "out of memory in commit step 1; the old generation continues");
    }

    for (const PendingUnit& u : P.units)
    {
        for (const std::string& w : u.mod_plan.warnings)
            std::fprintf(stderr, "[script] WARNING: %s\n", w.c_str());
        for (const PendingClass& pc : u.classes)
            for (const std::string& w : pc.plan.warnings)
                std::fprintf(stderr, "[script] WARNING: %s\n", w.c_str());
    }

    // Step 2: migrate. A throwing move constructor (or initializer) is fatal (design 6.6): the
    // runtime cannot undo a partial move of native objects. The runtime requests a restart.
    // If the handler returns, the old objects and the old generation stay as they are. The
    // new blocks are freed without a destructor call, because the partial state is unknown.
    // A move-only field (for example std::unique_ptr) of the old object is moved-from after
    // a failed migration. The old object keeps the field, but its value is empty.
    try
    {
        // The module blocks come first. A field initializer of a class can read a module variable.
        for (BlockWork& bw : blocks)
            if (const ModuleStateInfo& nms = bw.u->staged.module_state(); nms.layout)
            {
                *nms.slot = bw.new_mod;
                migrate_block(bw.had_old ? *bw.old.layout : kEmptyLayout, *nms.layout, bw.u->mod_plan,
                    bw.had_old ? bw.old.block : nullptr, bw.new_mod);
            }
        for (Work& w : work)
            migrate_block(*w.old_layout, *w.pc->incoming->layout, w.pc->plan, w.old_block, w.new_block);
    }
    catch (...)
    {
        free_new();
        pending_.reset();
        const std::string error = "FATAL: exception during state migration; a restart is needed";
        request_restart(error.c_str());
        return failed(ReloadReason::MigrationFailed, error);
    }
    for (Work& w : work)
        w.obj->state = w.new_block;

    // Step 4: swap the dispatch tables and the layouts. Add the new classes.
    for (PendingUnit& u : P.units)
        for (const PendingClass& pc : u.classes)
        {
            if (pc.canon)
            {
                ClassRecord* r = pc.canon->record;
                // A new constructor key: the "construct" of the new core becomes canonical. The shell
                // of the core uses the canonical record (init_state).
                if (u.have_core && has_new_ctor_key(r, pc.incoming))
                {
                    if (const ClassEntry* ce = u.core_reg.find(pc.incoming->name))
                    {
                        r->construct = ce->record->construct;
                        r->construct_key = ce->record->construct_key;
                        r->construct_keys = ce->record->construct_keys;
                        ce->record->canonical = r;
                    }
                }
                adopt(r, pc.incoming);
                adopt_ifaces(r, pc.incoming);
                removed_.erase(r->name); // the class is back; the table above replaced the handler table
            }
            else if (u.have_core)
            {
                // The record, the factory and the shell of the core module are canonical.
                const ClassEntry* ce = u.core_reg.find(pc.incoming->name);
                registry_.add(ce->record, ce->create);
                adopt(ce->record, pc.incoming);
                adopt_ifaces(ce->record, pc.incoming);
            }
            else
            {
                registry_.add(pc.incoming, pc.create);
                adopt_ifaces(pc.incoming, pc.incoming);
            }
            if (!pc.canon)
                unit_of_.emplace(pc.incoming->name, u.module.unit);
        }
    // A class that the new generation does not have is removed (design 6.1). The record stays
    // in the registry. Each slot points to the handler. The objects keep their state block.
    const auto in_batch = [&](const std::string& name)
    {
        for (const PendingUnit& u : P.units)
            if (u.staged.find(name))
                return true;
        return false;
    };
    for (const PendingUnit& u : P.units)
        for (auto& [name, entry] : registry_.entries())
        {
            if (in_batch(name) || removed_.count(name))
                continue;
            if (!u.module.unit.empty()) // a named unit removes only its own classes
            {
                const auto un = unit_of_.find(name);
                if (un == unit_of_.end() || un->second != u.module.unit)
                    continue;
            }
            ClassRecord* r = entry.record;
            std::vector<SlotFn>& table = removed_[name];
            table.assign(r->slot_count, reinterpret_cast<SlotFn>(&StateBase::__removed));
            r->slots = table.data();
            r->on_reload = nullptr;
            r->dtor = nullptr; // the script destructor belongs to a source that does not exist now
            r->new_default = nullptr; // the same: the factory makes an object without a constructor
            r->attributes = nullptr; // the text is in the module of a source that does not exist now
            r->slot_names = nullptr; // the same
            r->ctors = nullptr; // the same: only the factory makes an object of a removed class
            r->iface_rows = nullptr; // the rows are in the module; the tables of the loader stay
            r->ifaces = session_ifaces(r->ifaces); // the module of the record can unload
            size_t live = 0;
            {
                const LiveObjects::Guard guard(g_live);
                for (const auto& [s, creator] : g_live)
                    live += s->cls == r;
            }
            std::fprintf(stderr,
                "[script] WARNING: class %s is removed from the source; %zu live object(s) keep "
                "their state, and calls of its methods are ignored\n",
                name.c_str(), live);
        }
    // A new class with a script parent: "super" points to the record of the parent in the
    // registry, so a walk of the records sees the canonical records (design 4.5).
    for (const PendingUnit& u : P.units)
        for (const PendingClass& pc : u.classes)
        {
            if (pc.canon)
                continue;
            ClassRecord* r = registry_.find(pc.incoming->name)->record;
            if (!r->super && !r->super_name)
                continue;
            if (ClassEntry* pe = registry_.find(parent_name(r)))
                r->super = pe->record;
        }
    for (const PendingUnit& u : P.units)
        for (const PendingClass& pc : u.classes)
            if (pc.incoming->super_name)
                own_slots_[pc.incoming->name] = pc.incoming->slots;
    link_inherited_slots();
    // With a core, the new thunks come from the core and the generation holds no entry.
    std::vector<uint32_t> new_refs(P.units.size(), 0);
    for (size_t i = 0; i < P.units.size(); ++i)
    {
        PendingUnit& u = P.units[i];
        for (const PendingClass& pc : u.classes)
            if (!pc.canon && !u.have_core)
                ++new_refs[i];
        // A revert gives the AOT code back. The AOT thunks carry the signature keys of the AOT code.
        // The loop below sets these keys again, so a delegate of an override signature detaches.
        for (const auto& [key, t] : (u.have_core ? u.core_reg : u.staged).thunks())
        {
            if (!registry_.has_thunk(key) && !u.have_core)
                ++new_refs[i];
            // A thunk with a signature key (design 5.3): the signature of the function changed. The
            // slot gets the key, so a delegate on a thunk of another signature detaches.
            if (!t.sig.empty())
            {
                const std::string prev = registry_.current_thunk_key(t.name);
                if (!prev.empty() && prev != key)
                    std::fprintf(stderr,
                        "[script] WARNING: the signature of %s changed; native delegates bound to the "
                        "old signature detach (a call logs once and does not run)\n",
                        t.name.c_str());
                set_slot_key(t);
            }
            registry_.add_thunk(t);
        }
    }
    // The slot count can grow. The key table of each class follows it.
    for (auto& [cls, keys] : slot_keys_)
        if (ClassEntry* ce = registry_.find(cls))
            if (keys.size() < ce->record->slot_count)
                keys.resize(ce->record->slot_count, nullptr);
    refresh_slot_keys();
    for (PendingUnit& u : P.units)
        if (u.have_core)
            cores_.push_back(std::move(u.core));

    // Step 5: switch the generation and the links. The importers of a unit find the new module
    // block and the new function table by name. A new link epoch makes each access site resolve
    // again (FieldSite).
    uint32_t gen = 0;
    for (const PendingUnit& u : P.units)
        gen = std::max(gen, u.staged.max_generation());
    active_generation = gen;
    for (BlockWork& bw : blocks)
    {
        const std::string& unit = bw.u->module.unit;
        if (const ModuleStateInfo& nms = bw.u->staged.module_state(); nms.layout)
            mod_blocks_[unit] = {nms.layout, nms.slot, bw.new_mod};
        else
            mod_blocks_.erase(unit);
        switch_live(unit, bw.u->staged, bw.u->have_core ? &bw.u->core_reg : nullptr);
        set_link(bw.u->module, bw.u->staged);
    }
    ++link_epoch;

    // Step 5b: run the onReload hooks (design 4.7, 6.6). They run in the new generation. The
    // old blocks are still alive. A hook that throws is logged. The object stays valid.
    for (Work& w : work)
    {
        if (!w.obj->cls->on_reload)
            continue;
        try
        {
            w.obj->cls->on_reload(w.obj, OldState(w.old_layout, w.old_block));
        }
        catch (const std::exception& e)
        {
            std::fprintf(stderr, "[script] ERROR: onReload of %s threw: %s\n", w.obj->cls->name, e.what());
        }
        catch (...)
        {
            std::fprintf(stderr, "[script] ERROR: onReload of %s threw an unknown exception\n", w.obj->cls->name);
        }
    }

    // Step 6: destroy the old fields and free the old blocks.
    for (Work& w : work)
    {
        destroy_fields(w.old_layout, w.old_block);
        aligned_free(w.old_block);
    }
    for (BlockWork& bw : blocks)
    {
        if (!bw.had_old)
            continue;
        // The old generation must not read the freed block. A slot that the new module shares
        // (an AOT revert) keeps the new block.
        if (bw.old.slot && bw.old.slot != bw.u->staged.module_state().slot)
            *bw.old.slot = nullptr;
        destroy_fields(bw.old.layout, bw.old.block);
        aligned_free(bw.old.block);
    }

    // The loader owns the modules. The old generations of the units are retired now. The live
    // module of another unit keeps its code: its functions are still in use.
    std::vector<std::function<void()>> accepts;
    for (size_t i = 0; i < P.units.size(); ++i)
    {
        PendingUnit& u = P.units[i];
        const std::string unit = u.module.unit;
        std::fprintf(stderr, "[script] reload committed: %s, generation %u, %zu objects migrated\n",
            u.module.origin.c_str(), active_generation, work.size());
        for (Gen& g : gens_)
            if (g.state == GenState::Active && g.module.unit == unit)
            {
                g.state = GenState::Retired;
                g.retired_frame = frame_;
            }
        if (!u.have_core)
            note_records(u.staged, active_generation);
        if (u.module.on_accept)
            accepts.push_back(u.module.on_accept);
        add_generation(std::move(u.module), active_generation);
        gens_.back().registry_refs = new_refs[i];
    }
    pending_.reset();
    // The generation is live. The callbacks run under the loader mutex: they must not call the Loader.
    for (const std::function<void()>& on_accept : accepts)
        on_accept();
    return make_result(ReloadStatus::Applied, ReloadReason::None);
}

} // namespace hot

namespace hot
{
// Fills the inherited slots of each class with a script parent in another unit (modules step 5).
// A parent comes before its child, so a hole of a chain over three units gets the final function.
// A commit calls it after each adopt, also when only the parent unit changed.
void Loader::link_inherited_slots()
{
    std::set<std::string> done;
    std::function<void(const std::string&)> link = [&](const std::string& name)
    {
        if (!done.insert(name).second)
            return;
        const auto own = own_slots_.find(name);
        ClassEntry* e = registry_.find(name);
        if (own == own_slots_.end() || !e || removed_.count(name))
            return;
        ClassRecord* r = e->record;
        if (r->super)
            link(r->super->name);
        std::vector<SlotFn>& table = linked_slots_[name];
        table.assign(own->second, own->second + r->slot_count);
        for (uint32_t i = 0; i < r->slot_count; ++i)
            if (!table[i] && r->super && i < r->super->slot_count)
                table[i] = r->super->slots[i];
        r->slots = table.data();
    };
    for (const auto& [name, own] : own_slots_)
        link(name);
}
} // namespace hot
