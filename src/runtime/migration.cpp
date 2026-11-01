// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

#include "hot/runtime/migration.h"

#include "runtime_internal.h"

#include <cstdlib>
#include <cstring>
#include <cxxabi.h>

namespace hot
{

std::string readable_type(const char* type_key)
{
    if (!type_key)
        return "?";
    const std::string plain(type_key, std::strcspn(type_key, "#")); // without a layout hash
    int status = 0;
    char* d = abi::__cxa_demangle(plain.c_str(), nullptr, nullptr, &status);
    if (status != 0 || !d)
    {
        std::free(d);
        return plain;
    }
    std::string out = d;
    std::free(d);
    // The long names of the standard string types get their short names.
    static const std::pair<const char*, const char*> kShort[] = {
        {"std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >", "std::string"},
        {"std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char>>", "std::string"},
        {"std::basic_string<char, std::char_traits<char>, std::allocator<char> >", "std::string"},
        {"std::basic_string<char, std::char_traits<char>, std::allocator<char>>", "std::string"},
    };
    for (const auto& [from, to] : kShort)
        for (size_t at; (at = out.find(from)) != std::string::npos;)
            out.replace(at, std::strlen(from), to);
    return out;
}

std::string readable_layout(const char* layout_name)
{
    std::string n = layout_name ? layout_name : "?";
    const size_t at = n.rfind("State_g");
    if (at == std::string::npos || at == 0 || at + 7 >= n.size())
        return n;
    for (size_t i = at + 7; i < n.size(); ++i)
        if (n[i] < '0' || n[i] > '9')
            return n;
    return n.substr(0, at);
}

MigrationPlan build_plan(const StateLayout& o, const StateLayout& n)
{
    MigrationPlan plan;
    plan.steps.assign(n.field_count, FieldStep{FieldAction::Init, 0});
    std::vector<bool> claimed(o.field_count, false);
    std::vector<bool> done(n.field_count, false);

    auto match = [&](uint32_t i, uint32_t j)
    {
        const FieldInfo& nf = n.fields[i];
        const FieldInfo& of = o.fields[j];
        claimed[j] = true;
        done[i] = true;
        if (std::strcmp(nf.type_key, of.type_key) == 0)
        {
            if (nf.sub && of.sub)
            {
                // A [value] script class: the type name is the same, the layout may differ.
                auto sub = std::make_shared<MigrationPlan>(build_plan(*of.sub, *nf.sub));
                for (const std::string& w : sub->warnings)
                    plan.warnings.push_back(readable_layout(n.name) + "." + nf.name + ": " + w);
                plan.steps[i] = {FieldAction::Nested, j, std::move(sub)};
                return;
            }
            plan.steps[i] = {FieldAction::Move, j};
            return;
        }
        // The type changed (design 4.7): the convert hook, else the default value.
        plan.changed.push_back(j);
        // A native container of a [value] script class with a changed layout (design 5.3). The
        // runtime cannot migrate the elements of a native container, so the container resets.
        const char* nh = std::strchr(nf.type_key, '#');
        const char* oh = std::strchr(of.type_key, '#');
        if (nh && oh && nh - nf.type_key == oh - of.type_key
            && std::strncmp(nf.type_key, of.type_key, static_cast<size_t>(nh - nf.type_key)) == 0)
        {
            plan.warnings.push_back(readable_layout(n.name) + ": field " + nf.name + " (" +
                                    readable_type(nf.type_key) +
                                    ") holds a script value type whose layout changed; the native "
                                    "container resets to the default value");
            return;
        }
        if (nf.convert && nf.convert_from && std::strcmp(nf.convert_from, of.type_key) == 0)
        {
            plan.steps[i] = {FieldAction::Convert, j};
        }
        else
        {
            plan.warnings.push_back(readable_layout(n.name) + ": field " + nf.name + " changed its type ("
                + readable_type(of.type_key) + " -> " + readable_type(nf.type_key)
                + "); no matching convert hook, the default value is used");
        }
    };

    // Pass 1: same name.
    for (uint32_t i = 0; i < n.field_count; ++i)
        for (uint32_t j = 0; j < o.field_count; ++j)
            if (!claimed[j] && std::strcmp(n.fields[i].name, o.fields[j].name) == 0)
            {
                match(i, j);
                break;
            }
    // Pass 2: the "alias" name of a new field.
    for (uint32_t i = 0; i < n.field_count; ++i)
    {
        if (done[i] || !n.fields[i].alias)
            continue;
        for (uint32_t j = 0; j < o.field_count; ++j)
            if (!claimed[j] && std::strcmp(n.fields[i].alias, o.fields[j].name) == 0)
            {
                match(i, j);
                break;
            }
    }
    for (uint32_t j = 0; j < o.field_count; ++j)
        if (!claimed[j])
            plan.removed.push_back(j);

    // A removed field and a new field of the same type look like a rename without [alias].
    // The value is lost (design 5.3). Pair each removed field with one new field.
    std::vector<bool> paired(n.field_count, false);
    for (uint32_t j : plan.removed)
        for (uint32_t i = 0; i < n.field_count; ++i)
        {
            if (done[i] || paired[i] || std::strcmp(n.fields[i].type_key, o.fields[j].type_key) != 0)
                continue;
            paired[i] = true;
            plan.warnings.push_back(readable_layout(n.name) + ": field " + o.fields[j].name + " is removed and field "
                + n.fields[i].name + " is added. This looks like a rename; the value is lost. Add [alias(\""
                + o.fields[j].name + "\")] to keep it");
            break;
        }
    return plan;
}

// Fills a new block: moves and conversions first, then initializers in declaration order.
// The old block stays valid. The caller destroys its fields later.
void migrate_block(const StateLayout& o, const StateLayout& n, const MigrationPlan& plan, char* from, char* to)
{
    for (uint32_t i = 0; i < n.field_count; ++i)
    {
        const FieldStep& s = plan.steps[i];
        if (s.action == FieldAction::Init)
            continue;
        const FieldInfo& nf = n.fields[i];
        const FieldInfo& of = o.fields[s.old_index];
        if (s.action == FieldAction::Convert)
        {
            nf.convert(to + nf.offset, from + of.offset);
            continue;
        }
        if (s.action == FieldAction::Nested)
        {
            migrate_block(*of.sub, *nf.sub, *s.nested, from + of.offset, to + nf.offset);
            continue;
        }
        if (nf.flags & kNotMovable)
            continue; // the second loop runs the initializer
        if ((nf.flags & kTriviallyCopyable) && (of.flags & kTriviallyCopyable))
            std::memcpy(to + nf.offset, from + of.offset, nf.size);
        else
            nf.move(to + nf.offset, from + of.offset);
    }
    for (uint32_t i = 0; i < n.field_count; ++i)
    {
        const FieldInfo& nf = n.fields[i];
        const FieldAction a = plan.steps[i].action;
        if (a != FieldAction::Init && !(a == FieldAction::Move && (nf.flags & kNotMovable)))
            continue;
        if (nf.init)
            nf.init(to, to + nf.offset);
    }
}

} // namespace hot
