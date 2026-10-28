#include "spp/runtime/migration.h"

#include <cstring>

#include "runtime_internal.h"

namespace script {

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
        } else if (plan.error.empty()) {
            plan.error = std::string(n.name) + ": field " + nf.name + " changed its type (" +
                         of.type_key + " -> " + nf.type_key + ")";
        }
    };

    // Pass 1: same name.
    for (uint32_t i = 0; i < n.field_count; ++i)
        for (uint32_t j = 0; j < o.field_count; ++j)
            if (!claimed[j] && std::strcmp(n.fields[i].name, o.fields[j].name) == 0) {
                match(i, j);
                break;
            }
    // Pass 2: the "alias" name of a new field.
    for (uint32_t i = 0; i < n.field_count; ++i) {
        if (done[i] || !n.fields[i].alias) continue;
        for (uint32_t j = 0; j < o.field_count; ++j)
            if (!claimed[j] && std::strcmp(n.fields[i].alias, o.fields[j].name) == 0) {
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
void migrate_block(const StateLayout& o, const StateLayout& n, const MigrationPlan& plan,
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

}  // namespace script
