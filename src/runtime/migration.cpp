#include "spp/runtime/migration.h"

#include <cstring>

#include "runtime_internal.h"

namespace spp {

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
            return;
        }
        // The type changed (design 4.7): the convert hook, else the default value.
        plan.changed.push_back(j);
        if (nf.convert && nf.convert_from && std::strcmp(nf.convert_from, of.type_key) == 0) {
            plan.steps[i] = {FieldAction::Convert, j};
        } else {
            plan.warnings.push_back(std::string(n.name) + ": field " + nf.name +
                                    " changed its type (" + of.type_key + " -> " + nf.type_key +
                                    "); no matching convert hook, the default value is used");
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
    for (uint32_t j = 0; j < o.field_count; ++j)
        if (!claimed[j]) plan.removed.push_back(j);

    // A removed field and a new field of the same type look like a rename without [alias].
    // The value is lost (design 5.3). Pair each removed field with one new field.
    std::vector<bool> paired(n.field_count, false);
    for (uint32_t j : plan.removed)
        for (uint32_t i = 0; i < n.field_count; ++i) {
            if (done[i] || paired[i] || std::strcmp(n.fields[i].type_key, o.fields[j].type_key) != 0)
                continue;
            paired[i] = true;
            plan.warnings.push_back(std::string(n.name) + ": field " + o.fields[j].name +
                                    " is removed and field " + n.fields[i].name +
                                    " is added. This looks like a rename; the value is lost. Add [alias(\"" +
                                    o.fields[j].name + "\")] to keep it");
            break;
        }
    return plan;
}

// Fills a new block: moves and conversions first, then initializers in declaration order.
// The old block stays valid. The caller destroys its fields later.
void migrate_block(const StateLayout& o, const StateLayout& n, const MigrationPlan& plan,
                   char* from, char* to)
{
    for (uint32_t i = 0; i < n.field_count; ++i) {
        const FieldStep& s = plan.steps[i];
        if (s.action == FieldAction::Init) continue;
        const FieldInfo& nf = n.fields[i];
        const FieldInfo& of = o.fields[s.old_index];
        if (s.action == FieldAction::Convert) {
            nf.convert(to + nf.offset, from + of.offset);
            continue;
        }
        if ((nf.flags & kTriviallyCopyable) && (of.flags & kTriviallyCopyable))
            std::memcpy(to + nf.offset, from + of.offset, nf.size);
        else
            nf.move(to + nf.offset, from + of.offset);
    }
    for (uint32_t i = 0; i < n.field_count; ++i) {
        if (plan.steps[i].action != FieldAction::Init) continue;
        const FieldInfo& nf = n.fields[i];
        if (nf.init) nf.init(to, to + nf.offset);
    }
}

}  // namespace spp
