// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The storage of hot::weak_registry<T>. One map for the whole process, in the host.
#include "hot/runtime/weak_registry.h"

#include <mutex>
#include <string>
#include <unordered_map>

namespace hot::detail
{
namespace
{
std::mutex g_mutex;
using Objects = std::unordered_map<void*, std::shared_ptr<void>>;
std::unordered_map<std::string, Objects>& types()
{
    static std::unordered_map<std::string, Objects> m; // a function static: safe at start-up
    return m;
}
} // namespace

void weak_registry_add(const char* type_key, void* p, std::shared_ptr<void> keep)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    types()[type_key].try_emplace(p, std::move(keep));
}

void weak_registry_remove(const char* type_key, void* p)
{
    std::shared_ptr<void> dropped;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto t = types().find(type_key);
    if (t == types().end())
        return;
    auto it = t->second.find(p);
    if (it == t->second.end())
        return;
    dropped = std::move(it->second);
    t->second.erase(it);
}

std::shared_ptr<void> weak_registry_find(const char* type_key, void* p)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    auto t = types().find(type_key);
    if (t == types().end())
        return {};
    auto it = t->second.find(p);
    return it == t->second.end() ? std::shared_ptr<void>() : it->second;
}
} // namespace hot::detail
