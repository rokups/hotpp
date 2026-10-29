#include "spp/runtime/loader.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <exception>
#include <functional>
#include <iterator>
#include <set>
#include <utility>

#include "runtime_internal.h"

namespace spp {

struct PendingClass {
    ClassEntry* canon;  // null: the class is new
    ClassRecord* incoming;
    Factory create;
    MigrationPlan plan;
};

struct Loader::Pending {
    Module module;  // keeps the code alive; released with the Pending when the reload fails
    Registry staged;
    std::vector<PendingClass> classes;
    MigrationPlan mod_plan;  // the plan of the module state block (old layout to new layout)
    // The core instance (module.h) when the generation adds a class or a thunk, else empty.
    bool have_core = false;
    Module core;
    Registry core_reg;
};

namespace {
// The layout of a script without module variables: the plan of a new block is all "Init".
// A removed class also gets it when the generation of its last layout unloads.
const StateLayout kEmptyLayout = {"", 1, 1, 0, nullptr};

// Copies the parts of a generation record that change at a reload into the canonical record.
void adopt(ClassRecord* canon, ClassRecord* in)
{
    canon->slots = in->slots;
    canon->slot_count = in->slot_count;
    canon->layout = in->layout;
    canon->generation = in->generation;
    canon->on_reload = in->on_reload;
    canon->new_default = in->new_default;
    canon->dtor = in->dtor;
    if (in != canon) in->canonical = canon;
}

// Makes the core instance of "m" (module.h) and registers it in "reg". The core must register
// the same classes and thunks as "code", in other memory. On false there is no core and a
// warning tells why; then "m" keeps the parts. The function clears m.make_core in each case.
bool open_core(Module& m, const Registry& code, Module& core, Registry& reg)
{
    if (!m.make_core) return false;
    const std::function<LoadResult()> make = std::move(m.make_core);
    m.make_core = nullptr;
    std::string why;
    LoadResult r;
    try {
        r = make();
    } catch (...) {
        r.error = "make_core threw an exception";
    }
    if (!r || !r.module->register_fn) {
        why = r ? std::string("no script_register") : r.error;
    } else {
        try {
            r.module->register_fn(reg);
        } catch (...) {
            why = "script_register of the core threw an exception";
        }
    }
    if (why.empty()) {
        bool same = reg.entries().size() == code.entries().size() && reg.thunks().size() == code.thunks().size();
        for (const auto& [name, e] : code.entries()) {
            const ClassEntry* c = reg.find(name);
            same = same && c && c->record != e.record && c->record->generation == e.record->generation &&
                   c->record->shell_hash == e.record->shell_hash;
        }
        for (const auto& [key, t] : code.thunks()) {
            auto it = reg.thunks().find(key);
            same = same && it != reg.thunks().end() && it->second.fn && it->second.fn != t.fn;
        }
        if (!same) why = "the core does not register the same classes and thunks in its own memory";
    }
    if (!why.empty()) {
        std::fprintf(stderr, "[script] WARNING: no core module for %s (%s); the generation stays loaded\n",
                     m.origin.c_str(), why.c_str());
        return false;
    }
    core = std::move(*r.module);
    return true;
}
}  // namespace

namespace {
std::atomic<RestartHandler> g_restart_handler{nullptr};
std::atomic<bool> g_restart_requested{false};
}  // namespace

void set_restart_handler(RestartHandler h) { g_restart_handler.store(h); }
bool restart_requested() { return g_restart_requested.load(); }
void clear_restart_request() { g_restart_requested.store(false); }

void request_restart(const char* reason)
{
    g_restart_requested.store(true);
    std::fprintf(stderr, "[script] FATAL: restart requested: %s\n", reason);
    if (RestartHandler h = g_restart_handler.load()) {
        h(reason);
        return;
    }
    std::abort();  // no engine handler: the process cannot go on
}

Loader::Loader() = default;
Loader::~Loader()
{
    set_sync_mode(SyncMode::StopTheWorld);  // the mode is global; the next loader starts clean
    if (mod_block_) {  // the code of the field destroyers is still loaded: "gens_" goes later
        destroy_fields(mod_layout_, mod_block_);
        std::free(mod_block_);
    }
}

bool Loader::make_module_block(const ModuleStateInfo& info, std::string& error)
{
    char* block = alloc_state(info.layout);
    if (!block) {
        error = "out of memory for the module state";
        return false;
    }
    *info.slot = block;  // an initializer can read the block
    try {
        for (uint32_t i = 0; i < info.layout->field_count; ++i) {
            const FieldInfo& f = info.layout->fields[i];
            if (f.init) f.init(block, block + f.offset);
        }
    } catch (...) {
        *info.slot = nullptr;
        std::free(block);  // the partial state is unknown: no destructor call
        error = "an initializer of a module variable threw an exception";
        return false;
    }
    mod_layout_ = info.layout;
    mod_slot_ = info.slot;
    mod_block_ = block;
    return true;
}

bool Loader::load(Module module, std::string& error)
{
    if (!module.register_fn) {
        error = "the module has no script_register function";
        return false;
    }
    Registry code;
    module.register_fn(code);
    // The core instance gives the canonical records, the factories and the thunks (module.h).
    Module core;
    Registry core_reg;
    const bool have_core = open_core(module, code, core, core_reg);
    std::lock_guard<std::mutex> lock(mutex_);
    if (const ModuleStateInfo& ms = code.module_state(); ms.layout) {
        if (mod_block_) {
            error = "the loader has a module state block already";
            return false;
        }
        if (!make_module_block(ms, error)) return false;
    }
    const Registry& canon = have_core ? core_reg : code;
    for (const auto& [name, e] : canon.entries()) registry_.add(e.record, e.create);
    for (const auto& [key, t] : canon.thunks()) {
        registry_.add_thunk(t);
        if (!t.sig.empty()) set_slot_key(t);
    }
    if (have_core)
        for (const auto& [name, e] : code.entries()) adopt(registry_.find(name)->record, e.record);
    const uint32_t gen = registry_.max_generation();
    if (!have_core) note_records(code, gen);
    add_generation(std::move(module), gen);
    gens_.back().registry_refs =
        have_core ? 0u : static_cast<uint32_t>(code.entries().size() + code.thunks().size());
    if (have_core) cores_.push_back(std::move(core));
    active_generation = gen;
    return true;
}

// Remembers which module holds each record of a registry.
void Loader::note_records(const Registry& reg, uint32_t generation)
{
    for (const auto& [name, e] : reg.entries()) record_gen_.emplace(e.record, generation);
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
std::unordered_map<uint32_t, uint32_t> Loader::live_by_module() const
{
    std::unordered_map<uint32_t, uint32_t> live;
    for (const auto& [s, creator] : g_live) {
        uint32_t used[3] = {s->cls->generation, creator, 0};
        if (auto it = record_gen_.find(s->cls); it != record_gen_.end()) used[2] = it->second;
        for (int i = 0; i < 3; ++i) {
            const bool seen = (i > 0 && used[i] == used[0]) || (i > 1 && used[i] == used[1]);
            if (used[i] != 0 && !seen) ++live[used[i]];
        }
    }
    return live;
}

namespace {
// True when a field of the layout, or of a [value] struct in it, has the flag kMayHoldCode.
bool layout_holds_code(const StateLayout* l, int depth = 0)
{
    if (!l || depth > 32) return false;
    for (uint32_t i = 0; i < l->field_count; ++i) {
        const FieldInfo& f = l->fields[i];
        if (f.flags & kMayHoldCode) return true;
        if ((f.flags & kScriptStruct) && layout_holds_code(f.sub, depth + 1)) return true;
    }
    return false;
}
}  // namespace

bool Loader::state_holds_code() const
{
    if (mod_block_ && layout_holds_code(mod_layout_)) return true;
    std::unordered_map<const StateLayout*, bool> seen;
    for (const auto& [s, creator] : g_live) {
        auto [it, fresh] = seen.try_emplace(s->cls->layout, false);
        if (fresh) it->second = layout_holds_code(s->cls->layout);
        if (it->second) return true;
    }
    return false;
}

uint32_t Loader::code_refs(const Gen& g, const std::unordered_map<uint32_t, uint32_t>& live) const
{
    auto it = live.find(g.generation);
    return g.registry_refs + (it == live.end() ? 0 : it->second);
}

unsigned Loader::frame_boundary()
{
    unsigned unloaded = 0;
    std::unordered_map<uint32_t, uint32_t> live;
    bool have_live = false;
    for (Gen& g : gens_) {
        if (g.state != GenState::Retired || g.retired_frame >= frame_) continue;
        if (pin_count(g.generation) != 0) continue;
        if (!have_live) {
            // A field value can point to the code of any generation that ran (design 4.9).
            if (state_holds_code()) break;
            live = live_by_module(), have_live = true;
        }
        if (code_refs(g, live) != 0) continue;
        // A removed class without objects can still have its last layout in this generation.
        // A new object of it (the factory) gets an empty state then.
        for (const auto& [name, table] : removed_)
            if (ClassEntry* e = registry_.find(name); e && e->record->generation == g.generation)
                e->record->layout = &kEmptyLayout;
        for (auto it = record_gen_.begin(); it != record_gen_.end();)
            it = it->second == g.generation ? record_gen_.erase(it) : std::next(it);
        g.module = Module{};  // the keep-alive object releases the code
        g.state = GenState::Unloaded;
        std::fprintf(stderr, "[script] generation %u unloaded\n", g.generation);
        ++unloaded;
    }
    ++frame_;
    return unloaded;
}

LoaderStats Loader::stats() const
{
    LoaderStats st;
    st.active_generation = active_generation;
    st.live_objects = g_live.size();
    st.core_modules = cores_.size();
    st.state_holds_code = state_holds_code();
    const auto live = live_by_module();
    for (const Gen& g : gens_)
        st.generations.push_back({g.generation, g.state, pin_count(g.generation),
                                  code_refs(g, live), g.module.origin});
    std::unordered_map<const ClassRecord*, size_t> by_class;
    for (const auto& [s, creator] : g_live) ++by_class[s->cls];
    for (const auto& [name, e] : registry_.entries()) {
        const auto it = by_class.find(e.record);
        st.classes.push_back({name, e.record->generation, e.record->slot_count,
                              it == by_class.end() ? 0 : it->second, removed_.count(name) != 0});
        st.slots += e.record->slot_count;
    }
    std::sort(st.classes.begin(), st.classes.end(),
              [](const ClassInfo& a, const ClassInfo& b) { return a.name < b.name; });
    st.removed_class_calls = removed_class_call_count();
    return st;
}

std::string format_stats(const LoaderStats& st)
{
    static const char* const kState[] = {"active", "retired", "unloaded"};
    std::string o = "script_stats: active generation " + std::to_string(st.active_generation) + ", " +
                    std::to_string(st.live_objects) + " live objects, " + std::to_string(st.classes.size()) +
                    " classes, " + std::to_string(st.slots) + " slots\n";
    for (const GenerationInfo& g : st.generations)
        o += "  generation " + std::to_string(g.generation) + ": " + kState[static_cast<int>(g.state)] +
             ", pins " + std::to_string(g.pins) + ", code refs " + std::to_string(g.code_refs) + ", " +
             g.origin + "\n";
    for (const ClassInfo& c : st.classes)
        o += "  class " + c.name + ": generation " + std::to_string(c.generation) + ", " +
             std::to_string(c.slots) + " slots, " + std::to_string(c.live_objects) + " live" +
             (c.removed ? ", REMOVED" : "") + "\n";
    o += "  core modules: " + std::to_string(st.core_modules) + "\n";
    if (st.state_holds_code) o += "  unload blocked: a live state field can point to the code of a generation\n";
    o += "  removed class calls ignored: " + std::to_string(st.removed_class_calls) + "\n";
    return o;
}

namespace {
std::atomic<uint64_t> g_removed_calls{0};
}  // namespace

// The caller passes the arguments of the slot. The handler ignores them. The caller cleans the
// argument registers or the stack in the ABIs that this project supports.
void Loader::set_slot_key(const ThunkEntry& t)
{
    const std::string cls = t.name.substr(0, t.name.find("::"));
    ClassEntry* ce = registry_.find(cls);
    if (!ce) return;
    ClassRecord* r = ce->record;
    std::vector<const char*>& keys = slot_keys_[cls];
    const size_t n = std::max<size_t>(r->slot_count, t.slot + 1);
    if (keys.size() < n) keys.resize(n, nullptr);
    keys[t.slot] = key_text_.insert(t.sig).first->c_str();  // the module of "t" can unload
    r->slot_keys = keys.data();
}

void* removed_class_handler()
{
    if (g_removed_calls.fetch_add(1, std::memory_order_relaxed) == 0)
        std::fprintf(stderr,
                     "[script] call of a method of a removed script class ignored (the class is no "
                     "longer in the source); later calls are counted only\n");
    return nullptr;
}

uint64_t removed_class_call_count() { return g_removed_calls.load(std::memory_order_relaxed); }

bool Loader::request_reload(Module module, std::string& error)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pending_) {
            error = "a reload is already pending";
            return false;
        }
    }
    if (!module.register_fn) {
        error = "the module has no script_register function";
        return false;
    }
    auto p = std::make_unique<Pending>();
    p->module = std::move(module);
    try {
        p->module.register_fn(p->staged);
    } catch (...) {
        error = "script_register threw an exception";
        return false;
    }
    // A generation that adds a class or a thunk gets a core instance (module.h). Only a commit
    // changes "registry_", and a commit needs "pending_". The check of "pending_" below
    // rejects this request when another request was faster.
    bool adds = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [name, e] : p->staged.entries()) adds = adds || !registry_.find(name);
        for (const auto& [key, t] : p->staged.thunks()) adds = adds || !registry_.has_thunk(key);
    }
    if (adds) p->have_core = open_core(p->module, p->staged, p->core, p->core_reg);
    p->module.make_core = nullptr;  // the JIT keeps a copy of the IR for it

    // Validation. Nothing here changes a live class or a live object.
    // The lock keeps "registry_" and the class records stable. Only
    // commit_pending changes them, and it takes the same lock.
    std::lock_guard<std::mutex> lock(mutex_);
    if (pending_) {  // another thread was faster
        error = "a reload is already pending";
        return false;
    }
    // A class that is missing in the new generation is removed (design 6.1): no rejection.
    for (const auto& [name, in] : p->staged.entries()) {
        ClassEntry* canon = registry_.find(name);
        if (!in.record->layout) {
            error = "class " + name + " has no state layout";
            return false;
        }
        if (in.record->generation <= active_generation) {
            error = "class " + name + ": generation " + std::to_string(in.record->generation) +
                    " is not newer than the active generation " +
                    std::to_string(active_generation);
            return false;
        }
        PendingClass pc{canon, in.record, in.create, {}};
        if (canon) {
            const ClassRecord* cur = canon->record;
            const char* b0 = cur->base_name ? cur->base_name : "";
            const char* b1 = in.record->base_name ? in.record->base_name : "";
            if (std::strcmp(b0, b1) != 0) {
                // "base_name" lists the native bases: the primary base, then the interface bases
                // (design 4.4). An added or removed interface base changes the shell (5.3: RECONSTRUCT).
                const std::string s0 = b0, s1 = b1;
                if (s0.substr(0, s0.find(',')) == s1.substr(0, s1.find(',')))
                    error = "class " + name + ": native interface bases changed (" + s0 + " -> " + s1 +
                            "; RECONSTRUCT change)";
                else
                    error = "class " + name + ": base class changed (" + s0 + " -> " + s1 + ")";
                return false;
            }
            // A change of the script parent changes the shell hierarchy (design 5.3: RECONSTRUCT).
            const char* p0 = cur->super ? cur->super->name : "";
            const char* p1 = in.record->super ? in.record->super->name : "";
            if (std::strcmp(p0, p1) != 0) {
                error = "class " + name + ": script parent class changed (" + (*p0 ? p0 : "none") + " -> " +
                        (*p1 ? p1 : "none") + "; RECONSTRUCT change)";
                return false;
            }
            if (cur->shell_hash != in.record->shell_hash) {
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
                return false;
            }
            if (in.record->slot_count < cur->slot_count) {
                error = "class " + name + ": the new generation has fewer slots";
                return false;
            }
            // A type change does not reject the reload (design 4.7): the plan converts the
            // field or uses the default value. The commit logs the warnings of the plan.
            pc.plan = build_plan(*cur->layout, *in.record->layout);
        }
        p->classes.push_back(std::move(pc));
    }
    // The module state block migrates like the fields of a class (design 5.3). A type change
    // never rejects the reload. Without a block in the old generation, all variables are new.
    if (const ModuleStateInfo& nms = p->staged.module_state(); nms.layout) {
        if (!nms.slot) {
            error = "the module state has no slot";
            return false;
        }
        p->mod_plan = build_plan(mod_layout_ ? *mod_layout_ : kEmptyLayout, *nms.layout);
    }
    pending_ = std::move(p);
    return true;
}

void Loader::set_sync_mode(SyncMode mode)
{
    detail::g_entry_counters.store(kEntryCounters && mode == SyncMode::EntryCounters);
}

SyncMode Loader::sync_mode() const
{
    return entry_counters_enabled() ? SyncMode::EntryCounters : SyncMode::StopTheWorld;
}

bool Loader::commit_pending(std::string& error)
{
    error.clear();
    if (entry_counters_enabled()) {
        // EntryCounters mode (design 6.3). The thread must not wait for its own entry.
        if (entry_depth() != 0) {
            error = "commit_pending was called inside script code; the commit would wait for itself";
            return false;
        }
        if (!reload_pending()) return false;  // no window when nothing is pending
        CommitWindow window;  // no thread is in script code until the end of the scope
        std::lock_guard<std::mutex> lock(mutex_);
        return commit_locked(error);
    }
    // StopTheWorld mode. Debug check (design 6.3): all entry depths are zero. Count first,
    // then assert. In a release build kTickChecks is false and this code is gone.
    if constexpr (kTickChecks) {
        const int busy = in_script_tick();
        if (busy != 0) {
            g_unsafe_commits.fetch_add(1);
            std::fprintf(stderr, "[script] BUG: commit with %d thread(s) in script code\n", busy);
        }
        assert(busy == 0 && "commit_pending must run at the safe point");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    return commit_locked(error);
}

bool Loader::commit_locked(std::string& error)
{
    if (!pending_) return false;
    Pending& P = *pending_;

    struct Work {
        ShellBase* obj;
        const StateLayout* old_layout;
        char* old_block;
        char* new_block;
        const PendingClass* pc;
    };
    std::vector<Work> work;
    const ModuleStateInfo& nms = P.staged.module_state();
    char* new_mod = nullptr;

    // Step 1: allocate all new blocks. A failure here changes nothing.
    try {
        if (nms.layout) {
            new_mod = alloc_state(nms.layout);
            if (!new_mod) throw std::bad_alloc();
        }
        work.reserve(g_live.size());
        for (const auto& [s, creator] : g_live) {
            const PendingClass* pc = nullptr;
            for (const PendingClass& c : P.classes)
                if (c.canon && c.canon->record == s->cls) pc = &c;
            if (!pc) continue;
            char* b = alloc_state(pc->incoming->layout);
            if (!b) throw std::bad_alloc();
            work.push_back({s, s->cls->layout, static_cast<char*>(s->state), b, pc});
        }
    } catch (...) {
        for (Work& w : work) std::free(w.new_block);
        std::free(new_mod);
        error = "out of memory in commit step 1; the old generation continues";
        pending_.reset();
        return false;
    }

    for (const std::string& w : P.mod_plan.warnings)
        std::fprintf(stderr, "[script] WARNING: %s\n", w.c_str());
    for (const PendingClass& pc : P.classes)
        for (const std::string& w : pc.plan.warnings)
            std::fprintf(stderr, "[script] WARNING: %s\n", w.c_str());

    // Step 2: migrate. A throwing move constructor (or initializer) is fatal (design 6.6): the
    // runtime cannot undo a partial move of native objects. The runtime requests a restart.
    // If the handler returns, the old objects and the old generation stay as they are. The
    // new blocks are freed without a destructor call, because the partial state is unknown.
    try {
        // The module block comes first. A field initializer of a class can read a module variable.
        if (nms.layout) {
            *nms.slot = new_mod;
            migrate_block(mod_layout_ ? *mod_layout_ : kEmptyLayout, *nms.layout, P.mod_plan,
                          mod_block_, new_mod);
        }
        for (Work& w : work)
            migrate_block(*w.old_layout, *w.pc->incoming->layout, w.pc->plan, w.old_block,
                          w.new_block);
    } catch (...) {
        for (Work& w : work) std::free(w.new_block);
        if (nms.layout) {
            *nms.slot = nullptr;
            std::free(new_mod);
        }
        pending_.reset();
        error = "FATAL: exception during state migration; a restart is needed";
        request_restart(error.c_str());
        return false;
    }
    for (Work& w : work) w.obj->state = w.new_block;

    // Step 4: swap the dispatch table and the layout. Add the new classes.
    for (const PendingClass& pc : P.classes) {
        if (pc.canon) {
            ClassRecord* r = pc.canon->record;
            adopt(r, pc.incoming);
            removed_.erase(r->name);  // the class is back; the table above replaced the handler table
        } else if (P.have_core) {
            // The record, the factory and the shell of the core module are canonical.
            const ClassEntry* ce = P.core_reg.find(pc.incoming->name);
            registry_.add(ce->record, ce->create);
            adopt(ce->record, pc.incoming);
        } else {
            registry_.add(pc.incoming, pc.create);
        }
    }
    // A class that the new generation does not have is removed (design 6.1). The record stays
    // in the registry. Each slot points to the handler. The objects keep their state block.
    for (auto& [name, entry] : registry_.entries()) {
        if (P.staged.find(name) || removed_.count(name)) continue;
        ClassRecord* r = entry.record;
        std::vector<void*>& table = removed_[name];
        table.assign(r->slot_count, reinterpret_cast<void*>(&removed_class_handler));
        r->slots = table.data();
        r->on_reload = nullptr;
        r->dtor = nullptr;  // the script destructor belongs to a source that does not exist now
        r->new_default = nullptr;  // the same: the factory makes an object without a constructor
        size_t live = 0;
        for (const auto& [s, creator] : g_live) live += s->cls == r;
        std::fprintf(stderr,
                     "[script] WARNING: class %s is removed from the source; %zu live object(s) keep "
                     "their state, and calls of its methods are ignored\n",
                     name.c_str(), live);
    }
    // A new class with a script parent: "super" points to the record of the parent in the
    // registry, so a walk of the records sees the canonical records (design 4.5).
    for (const PendingClass& pc : P.classes) {
        if (pc.canon) continue;
        ClassRecord* r = registry_.find(pc.incoming->name)->record;
        if (!r->super) continue;
        if (ClassEntry* pe = registry_.find(r->super->name)) r->super = pe->record;
    }
    // With a core, the new thunks come from the core and the generation holds no entry.
    uint32_t new_refs = 0;
    for (const PendingClass& pc : P.classes)
        if (!pc.canon && !P.have_core) ++new_refs;
    for (const auto& [key, t] : (P.have_core ? P.core_reg : P.staged).thunks()) {
        if (!registry_.has_thunk(key) && !P.have_core) ++new_refs;
        // A thunk with a signature key (design 5.3): the signature of the function changed. The
        // slot gets the key, so a delegate on a thunk of another signature detaches.
        if (!t.sig.empty()) {
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
    // The slot count can grow. The key table of each class follows it.
    for (auto& [cls, keys] : slot_keys_)
        if (ClassEntry* ce = registry_.find(cls)) {
            if (keys.size() < ce->record->slot_count) keys.resize(ce->record->slot_count, nullptr);
            ce->record->slot_keys = keys.data();
        }
    if (P.have_core) cores_.push_back(std::move(P.core));

    // Step 5: switch the generation.
    active_generation = P.staged.max_generation();

    // Step 5b: run the onReload hooks (design 4.7, 6.6). They run in the new generation. The
    // old blocks are still alive. A hook that throws is logged. The object stays valid.
    for (Work& w : work) {
        if (!w.obj->cls->on_reload) continue;
        try {
            w.obj->cls->on_reload(w.obj, OldState(w.old_layout, w.old_block));
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[script] ERROR: onReload of %s threw: %s\n",
                         w.obj->cls->name, e.what());
        } catch (...) {
            std::fprintf(stderr, "[script] ERROR: onReload of %s threw an unknown exception\n",
                         w.obj->cls->name);
        }
    }

    // Step 6: destroy the old fields and free the old blocks.
    for (Work& w : work) {
        destroy_fields(w.old_layout, w.old_block);
        std::free(w.old_block);
    }
    if (mod_block_) {
        *mod_slot_ = nullptr;  // the old generation must not read the freed block
        destroy_fields(mod_layout_, mod_block_);
        std::free(mod_block_);
    }
    mod_layout_ = nms.layout;
    mod_slot_ = nms.slot;
    mod_block_ = new_mod;

    std::fprintf(stderr, "[script] reload committed: %s, generation %u, %zu objects migrated\n",
                 P.module.origin.c_str(), active_generation, work.size());
    // The loader owns the module. The old generations are retired now.
    for (Gen& g : gens_)
        if (g.state == GenState::Active) {
            g.state = GenState::Retired;
            g.retired_frame = frame_;
        }
    if (!P.have_core) note_records(P.staged, active_generation);
    add_generation(std::move(P.module), active_generation);
    gens_.back().registry_refs = new_refs;
    pending_.reset();
    return true;
}

}  // namespace spp
