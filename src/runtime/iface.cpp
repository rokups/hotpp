// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The ids of the interface methods and the errors of an interface call (hot/runtime/iface.h).
#include "hot/runtime/iface.h"

#include <mutex>
#include <string>
#include <unordered_map>

namespace hot
{

uint32_t iface_method_id(const char* key, const char* sig)
{
    static std::mutex mutex;
    static std::unordered_map<std::string, uint32_t> ids;
    std::string k(key);
    k += '\0';
    k += sig;
    std::lock_guard<std::mutex> lock(mutex);
    // The ids start at 1, so 0 can mean "not resolved yet" in a call site.
    return ids.emplace(std::move(k), static_cast<uint32_t>(ids.size() + 1)).first->second;
}

namespace detail
{
void iface_null_handle(const char* key)
{
    throw ScriptError("null interface handle '" + iface_of_key(key) + "'");
}

void iface_not_implemented(const ClassRecord* cls, const char* key)
{
    throw ScriptError(std::string("class '") + cls->name + "' does not implement '" + key + "'");
}
} // namespace detail

} // namespace hot
