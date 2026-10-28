#include "script_runtime.h"

#include <cstdio>
#include <unordered_set>

namespace script {

uint32_t active_generation = 0;

static std::unordered_set<ShellBase*> g_live;

void track_object(ShellBase* self) { g_live.insert(self); }
void untrack_object(ShellBase* self) { g_live.erase(self); }

void stale_call(const char* what)
{
    std::fprintf(stderr, "[script] stale call ignored: %s (active generation %u)\n", what,
                 active_generation);
}

// ------------------------------------------------------------ registry
void Registry::add(ClassRecord* record, Factory create)
{
    entries_[record->name] = ClassEntry{record, create};
    if (record->generation > max_generation_) max_generation_ = record->generation;
}

void Registry::add_thunk(const std::string& name, void* fn)
{
    thunks_.emplace(name, fn);  // the first address stays
}

const ClassEntry* Registry::find(const std::string& name) const
{
    auto it = entries_.find(name);
    return it == entries_.end() ? nullptr : &it->second;
}

ClassEntry* Registry::find(const std::string& name)
{
    auto it = entries_.find(name);
    return it == entries_.end() ? nullptr : &it->second;
}

void* Registry::thunk(const std::string& name) const
{
    auto it = thunks_.find(name);
    return it == thunks_.end() ? nullptr : it->second;
}

// ------------------------------------------------------- migration plan
MigrationPlan build_plan(const StateLayout& o, const StateLayout& n)
{
    MigrationPlan plan;
    plan.steps.assign(n.field_count, FieldStep{FieldAction::Init, 0});
    std::vector<bool> claimed(o.field_count, false);
    std::vector<bool> done(n.field_count, false);

    auto match = [&](uint32_t i, uint32_t j) {
        const FieldInfo& nf = n.fields[i];
        const FieldInfo& of = o.fields[j];
        claimed[j] = true;
        done[i] = true;
        if (std::strcmp(nf.type_key, of.type_key) == 0) {
            plan.steps[i] = {FieldAction::Move, j};
        } else {
            plan.steps[i] = {FieldAction::InitTypeChanged, j};
            plan.warnings.push_back(std::string(n.name) + ": field " + nf.name +
                                    " changed its type (" + of.type_key + " -> " + nf.type_key +
                                    "); the initializer runs");
        }
    };

    // Pass 1: same name.
    for (uint32_t i = 0; i < n.field_count; ++i)
        for (uint32_t j = 0; j < o.field_count; ++j)
            if (!claimed[j] && std::strcmp(n.fields[i].name, o.fields[j].name) == 0) {
                match(i, j);
                break;
            }
    // Pass 2: the "was" name of a new field.
    for (uint32_t i = 0; i < n.field_count; ++i) {
        if (done[i] || !n.fields[i].was) continue;
        for (uint32_t j = 0; j < o.field_count; ++j)
            if (!claimed[j] && std::strcmp(n.fields[i].was, o.fields[j].name) == 0) {
                match(i, j);
                break;
            }
    }
    for (uint32_t j = 0; j < o.field_count; ++j) {
        bool moved = false;
        for (const FieldStep& s : plan.steps)
            if (s.action == FieldAction::Move && s.old_index == j) moved = true;
        if (!moved) plan.removed.push_back(j);
    }
    return plan;
}

// Fills a new block: moves first, then initializers in declaration order.
// The old block stays valid. The caller destroys its fields later.
static void migrate_block(const StateLayout& o, const StateLayout& n, const MigrationPlan& plan,
                          char* from, char* to)
{
    for (uint32_t i = 0; i < n.field_count; ++i) {
        const FieldStep& s = plan.steps[i];
        if (s.action != FieldAction::Move) continue;
        const FieldInfo& nf = n.fields[i];
        const FieldInfo& of = o.fields[s.old_index];
        if ((nf.flags & kTriviallyCopyable) && (of.flags & kTriviallyCopyable))
            std::memcpy(to + nf.offset, from + of.offset, nf.size);
        else
            nf.move(to + nf.offset, from + of.offset);
    }
    for (uint32_t i = 0; i < n.field_count; ++i) {
        if (plan.steps[i].action == FieldAction::Move) continue;
        const FieldInfo& nf = n.fields[i];
        if (nf.init) nf.init(to, to + nf.offset);
    }
}

// -------------------------------------------------------------- loader
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

Loader::Loader() = default;
Loader::~Loader() = default;

bool Loader::load(Module module, std::string& error)
{
    if (!module.register_fn) {
        error = "the module has no script_register function";
        return false;
    }
    module.register_fn(registry_);
    modules_.push_back(std::move(module));
    active_generation = registry_.max_generation();
    return true;
}

bool Loader::request_reload(Module module, std::string& error)
{
    if (pending_) {
        error = "a reload is already pending";
        return false;
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
            if (cur->shell_hash != in.record->shell_hash) {
                error = "class " + name + ": shell layout hash differs (RECONSTRUCT change)";
                return false;
            }
            if (in.record->slot_count < cur->slot_count) {
                error = "class " + name + ": the new generation has fewer slots";
                return false;
            }
            pc.plan = build_plan(*cur->layout, *in.record->layout);
            for (const std::string& w : pc.plan.warnings)
                std::fprintf(stderr, "[script] warning: %s\n", w.c_str());
        }
        p->classes.push_back(std::move(pc));
    }
    pending_ = std::move(p);
    return true;
}

bool Loader::commit_pending(std::string& error)
{
    error.clear();
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

    // Step 2: migrate. A throwing move constructor is fatal.
    try {
        for (Work& w : work)
            migrate_block(*w.old_layout, *w.pc->incoming->layout, w.pc->plan, w.old_block,
                          w.new_block);
    } catch (...) {
        std::fprintf(stderr, "[script] FATAL: exception during state migration; restart\n");
        std::abort();
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
        } else {
            registry_.add(pc.incoming, pc.create);
        }
    }
    for (const auto& [name, fn] : P.staged.thunks()) registry_.add_thunk(name, fn);

    // Step 5: switch the generation.
    active_generation = P.staged.max_generation();

    // Step 6: destroy the old fields and free the old blocks.
    for (Work& w : work) {
        destroy_fields(w.old_layout, w.old_block);
        std::free(w.old_block);
    }

    std::fprintf(stderr, "[script] reload committed: %s, generation %u, %zu objects migrated\n",
                 P.module.origin.c_str(), active_generation, work.size());
    modules_.push_back(std::move(P.module));  // a retired generation stays loaded
    pending_.reset();
    return true;
}

}  // namespace script
