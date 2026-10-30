#include "hot/runtime/registry.h"

#include "hot/runtime/shell.h"
#include "hot/runtime/state.h"

#include <atomic>
#include <cstdio>
#include <mutex>
#include <set>
#include <utility>

namespace hot
{

namespace
{
std::atomic<uint64_t> g_detached_calls{0};
std::mutex g_detached_mutex;
std::set<std::pair<std::string, unsigned>> g_detached_logged;
} // namespace

void detached_call(const ClassRecord* cls, unsigned slot)
{
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

} // namespace hot
