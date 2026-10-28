#include "spp/runtime/loader.h"

#include <atomic>
#include <cassert>
#include <cstdio>
#include <exception>

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
};

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
Loader::~Loader() = default;

bool Loader::load(Module module, std::string& error)
{
    if (!module.register_fn) {
        error = "the module has no script_register function";
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    module.register_fn(registry_);
    const uint32_t gen = registry_.max_generation();
    note_records(registry_, gen);
    add_generation(std::move(module), gen);
    gens_.back().registry_refs =
        static_cast<uint32_t>(registry_.entries().size() + registry_.thunks().size());
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

std::unordered_map<uint32_t, uint32_t> Loader::live_by_module() const
{
    std::unordered_map<uint32_t, uint32_t> live;
    for (const ShellBase* s : g_live) {
        auto it = record_gen_.find(s->cls);
        if (it != record_gen_.end()) ++live[it->second];
    }
    return live;
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
        if (!have_live) live = live_by_module(), have_live = true;
        if (code_refs(g, live) != 0) continue;
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
    const auto live = live_by_module();
    for (const Gen& g : gens_)
        st.generations.push_back({g.generation, g.state, pin_count(g.generation),
                                  code_refs(g, live), g.module.origin});
    return st;
}

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

    // Validation. Nothing here changes a live class or a live object.
    // The lock keeps "registry_" and the class records stable. Only
    // commit_pending changes them, and it takes the same lock.
    std::lock_guard<std::mutex> lock(mutex_);
    if (pending_) {  // another thread was faster
        error = "a reload is already pending";
        return false;
    }
    for (const auto& [name, cur] : registry_.entries()) {
        if (!p->staged.find(name)) {
            error = "class " + name + " is missing in the new generation";
            return false;
        }
    }
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
                error = "class " + name + ": base class changed (" + b0 + " -> " + b1 + ")";
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
    pending_ = std::move(p);
    return true;
}

bool Loader::commit_pending(std::string& error)
{
    error.clear();
    // Debug check: no worker is in script code now. Count first, then assert.
    const int busy = in_script_tick();
    if (busy != 0) {
        g_unsafe_commits.fetch_add(1);
        std::fprintf(stderr, "[script] BUG: commit with %d thread(s) in script code\n", busy);
    }
    assert(busy == 0 && "commit_pending must run at the safe point");
    std::lock_guard<std::mutex> lock(mutex_);
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

    // Step 1: allocate all new blocks. A failure here changes nothing.
    try {
        work.reserve(g_live.size());
        for (ShellBase* s : g_live) {
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
        error = "out of memory in commit step 1; the old generation continues";
        pending_.reset();
        return false;
    }

    for (const PendingClass& pc : P.classes)
        for (const std::string& w : pc.plan.warnings)
            std::fprintf(stderr, "[script] WARNING: %s\n", w.c_str());

    // Step 2: migrate. A throwing move constructor (or initializer) is fatal (design 6.6): the
    // runtime cannot undo a partial move of native objects. The runtime requests a restart.
    // If the handler returns, the old objects and the old generation stay as they are. The
    // new blocks are freed without a destructor call, because the partial state is unknown.
    try {
        for (Work& w : work)
            migrate_block(*w.old_layout, *w.pc->incoming->layout, w.pc->plan, w.old_block,
                          w.new_block);
    } catch (...) {
        for (Work& w : work) std::free(w.new_block);
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
            r->slots = pc.incoming->slots;
            r->slot_count = pc.incoming->slot_count;
            r->layout = pc.incoming->layout;
            r->generation = pc.incoming->generation;
            r->on_reload = pc.incoming->on_reload;
            r->ctor_default = pc.incoming->ctor_default;
            r->dtor = pc.incoming->dtor;
            pc.incoming->canonical = r;
        } else {
            registry_.add(pc.incoming, pc.create);
        }
    }
    // A new class with a script parent: "super" points to the record of the parent in the
    // registry, so a walk of the records sees the canonical records (design 4.5).
    for (const PendingClass& pc : P.classes) {
        if (pc.canon || !pc.incoming->super) continue;
        if (ClassEntry* pe = registry_.find(pc.incoming->super->name)) pc.incoming->super = pe->record;
    }
    uint32_t new_refs = 0;
    for (const PendingClass& pc : P.classes)
        if (!pc.canon) ++new_refs;
    for (const auto& [name, fn] : P.staged.thunks()) {
        if (!registry_.thunk(name)) ++new_refs;
        registry_.add_thunk(name, fn);
    }

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

    std::fprintf(stderr, "[script] reload committed: %s, generation %u, %zu objects migrated\n",
                 P.module.origin.c_str(), active_generation, work.size());
    // The loader owns the module. The old generations are retired now.
    for (Gen& g : gens_)
        if (g.state == GenState::Active) {
            g.state = GenState::Retired;
            g.retired_frame = frame_;
        }
    note_records(P.staged, active_generation);
    add_generation(std::move(P.module), active_generation);
    gens_.back().registry_refs = new_refs;
    pending_.reset();
    return true;
}

}  // namespace spp
