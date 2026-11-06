// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

#include "hot/runtime/registry.h"

#include "hot/runtime/errors.h"
#include "hot/runtime/shell.h"
#include "hot/runtime/state.h"

#include <atomic>
#include <cstdio>
#include <mutex>
#include <set>
#include <unordered_map>
#include <utility>

namespace hot
{

namespace
{
std::atomic<uint64_t> g_detached_calls{0};
std::atomic<uint64_t> g_removed_calls{0};
std::mutex g_detached_mutex;
std::set<std::pair<std::string, unsigned>> g_detached_logged;
std::set<std::pair<std::string, unsigned>> g_removed_fn_logged; // under g_detached_mutex
std::atomic<bool> g_removed_logged{false};

// This reference keeps errors.cpp in the link. The host must export the symbols of errors.cpp
// to the script modules, but no host code names them. The linker would drop errors.o.
#if defined(__GNUC__) || defined(__clang__)
[[gnu::used]]
#endif
void (*const g_keep_errors)(const char*) = &detail::throw_null_reference; // cl: the whole archive keeps it

// The resolvers of the cross-module access and the cache of class_ref. The cache keeps the canonical
// record of a class by name. A record lives as long as the session.
std::mutex g_resolver_mutex;
detail::ClassResolver g_class_resolver = nullptr;
detail::UnitResolver g_unit_resolver = nullptr;
std::unordered_map<std::string, ClassRecord*> g_class_cache;
std::atomic<uint64_t> g_fn_table_id{0};
} // namespace

void note_removed_call()
{
    g_removed_calls.fetch_add(1, std::memory_order_relaxed);
    if (!g_removed_logged.exchange(true, std::memory_order_relaxed))
        std::fprintf(stderr,
            "[script] call of a method of a removed script class or function ignored (it is no "
            "longer in the source); later calls are counted only\n");
}

void removed_function_called(const ClassRecord* cls, unsigned slot)
{
    const char* const* names = cls->slot_names;
    const std::string what = names && names[slot] ? std::string(names[slot])
                                                  : "slot " + std::to_string(slot) + " of " + cls->name;
    g_removed_calls.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(g_detached_mutex);
        if (g_removed_fn_logged.emplace(cls->name, slot).second)
            std::fprintf(stderr,
                "[script] call of removed script function %s (it is no longer in the source); the call "
                "throws ScriptError\n",
                what.c_str());
    }
    throw ScriptError("call of removed script function " + what);
}

uint64_t removed_call_count()
{
    return g_removed_calls.load(std::memory_order_relaxed);
}

void detached_call(const ClassRecord* cls, unsigned slot)
{
    if (slot_is_removed(cls, slot))
    {
        note_removed_call(); // a removed function or class: no warning about a signature
        return;
    }
    g_detached_calls.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(g_detached_mutex);
    if (g_detached_logged.emplace(cls->name, slot).second)
        std::fprintf(stderr,
            "[script] WARNING: a native delegate calls slot %u of %s with an old signature; the "
            "delegate is detached and the call does not run\n",
            slot, cls->name);
}

const char* layout_keyed(const char* type, const char* hash)
{
    static std::mutex m;
    static std::set<std::string> text; // node addresses are stable
    std::lock_guard<std::mutex> lock(m);
    return text.insert(std::string(type) + "#" + hash).first->c_str();
}

uint64_t detached_call_count()
{
    return g_detached_calls.load(std::memory_order_relaxed);
}

void Registry::add(ClassRecord* record, Factory create)
{
    entries_[record->name] = ClassEntry{record, create};
    if (record->generation > max_generation_)
        max_generation_ = record->generation;
}

void Registry::add_fn(const std::string& name, const char* sig_key, void* fn)
{
    fns_.entries.insert_or_assign(name, FnEntry{name, sig_key ? sig_key : "", fn});
}

void Registry::add_thunk(const std::string& name, void* fn)
{
    add_thunk(ThunkEntry{fn, name, {}, 0});
}

void Registry::add_thunk(const std::string& name, void* fn, const char* sig, unsigned slot)
{
    add_thunk(ThunkEntry{fn, name, sig ? sig : "", slot});
}

void Registry::add_thunk(const ThunkEntry& entry)
{
    const std::string key = thunk_key(entry.name, entry.sig);
    thunks_.emplace(key, entry); // the first address stays
    current_[entry.name] = key;
}

std::string Registry::current_thunk_key(const std::string& name) const
{
    auto it = current_.find(name);
    return it == current_.end() ? std::string() : it->second;
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
    auto cur = current_.find(name);
    auto it = thunks_.find(cur == current_.end() ? name : cur->second);
    return it == thunks_.end() ? nullptr : it->second.fn;
}

namespace detail
{

uint64_t next_fn_table_id()
{
    return g_fn_table_id.fetch_add(1, std::memory_order_relaxed) + 1;
}

void set_class_resolver(ClassResolver fn)
{
    std::lock_guard<std::mutex> lock(g_resolver_mutex);
    g_class_resolver = fn;
    g_class_cache.clear();
}

void set_unit_resolver(UnitResolver fn)
{
    std::lock_guard<std::mutex> lock(g_resolver_mutex);
    g_unit_resolver = fn;
}

ClassRecord* resolve_class(const char* name)
{
    ClassResolver resolver = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_resolver_mutex);
        auto it = g_class_cache.find(name);
        if (it != g_class_cache.end())
            return it->second;
        resolver = g_class_resolver;
    }
    // The resolver runs without the lock: it can take locks of the loader.
    ClassRecord* rec = resolver ? resolver(name) : nullptr;
    if (!rec)
        throw ScriptError(std::string("unknown script class '") + name + "' of another unit");
    if (rec->canonical)
        rec = rec->canonical;
    std::lock_guard<std::mutex> lock(g_resolver_mutex);
    if (g_class_resolver == resolver)
        g_class_cache[name] = rec;
    return rec;
}

UnitInfo resolve_unit(const char* name)
{
    UnitResolver resolver;
    {
        std::lock_guard<std::mutex> lock(g_resolver_mutex);
        resolver = g_unit_resolver;
    }
    UnitInfo info;
    if (!resolver || !resolver(name, info))
        throw ScriptError(std::string("unknown script unit '") + name + "'");
    return info;
}

uint32_t resolve_field_offset(const StateLayout* layout, const char* name, const char* type, const char* owner)
{
    if (!layout)
        throw ScriptError(std::string(owner) + " has no state layout for the field '" + name + "'");
    for (uint32_t i = 0; i < layout->field_count; ++i)
    {
        const FieldInfo& f = layout->fields[i];
        if (std::strcmp(f.name, name) != 0)
            continue;
        if (!type_key_matches(f.type_key, type))
            throw ScriptError(std::string("the field '") + owner + "." + name + "' has type key '" +
                f.type_key + "', not '" + type + "'");
        return f.offset;
    }
    throw ScriptError(std::string(owner) + " has no field '" + name + "'");
}

void* resolve_fn(const UnitRef& unit, const char* name, const char* sig_key)
{
    if (!unit.info.fns)
        throw ScriptError(std::string("unit '") + unit.name + "' has no function table");
    const FnEntry* e = unit.info.fns->find(name);
    if (!e)
        throw ScriptError(std::string("unit '") + unit.name + "' has no function '" + name + "'");
    if (e->sig != sig_key)
        throw ScriptError(std::string("the function '") + unit.name + "." + name + "' has signature key '" +
            e->sig.c_str() + "', not '" + sig_key + "'");
    return e->fn;
}

ShellCore* construct_raw(const ClassRef& cls, const char* key, void** args, size_t count, uint64_t const_mask)
{
    const ClassRecord* rec = cls.record;
    if (!rec)
        throw ScriptError(std::string("construct of the null class reference '") + (cls.name ? cls.name : "") + "'");
    const CtorEntry* found = nullptr;
    if (rec->ctors)
        for (const CtorEntry* c = rec->ctors; c->key; ++c)
            if (std::strcmp(c->key, key) == 0)
            {
                found = c;
                break;
            }
    if (!found)
    {
        if (count == 0 && rec->new_default)
            return rec->new_default();
        throw ScriptError(std::string("class ") + rec->name + " has no constructor for the arguments (" +
            key + ")");
    }
    if (found->write_mask & const_mask)
        throw ScriptError(std::string("a constructor of class ") + rec->name +
            " changes an argument that is const (" + key + ")");
    return found->make(args).shell;
}

} // namespace detail

} // namespace hot
