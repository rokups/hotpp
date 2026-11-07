// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// hot::weak_registry<T>: a weak source for a native class that no handle owns.
// The host calls add(p) when the object starts to live and remove(p) before it dies.
// A script pointer field "T*" is then a hot::CheckedPtr<T>. It reads null after remove(p).
// A stub marks the class: [native, weak_source("hot::registered_weak_ref<$T>")] class T;
// Each class has one registry for the whole process. The host calls add and remove for each object.
#pragma once
#include "hot/runtime/checked_ptr.h"

#include <memory>
#include <mutex>
#include <unordered_map>

namespace hot
{

template <class T> class weak_registry
{
public:
    // Starts to track the object. A second call for the same object changes nothing.
    static void add(T* p)
    {
        if (!p)
            return;
        std::lock_guard<std::mutex> lock(mutex());
        // The owner is the registry. The deleter does nothing: the host owns the object.
        map().try_emplace(p, std::shared_ptr<T>(p, [](T*) {}));
    }

    // Stops to track the object. Each weak pointer to it reads null from now on.
    static void remove(T* p)
    {
        std::shared_ptr<T> dropped;
        {
            std::lock_guard<std::mutex> lock(mutex());
            auto it = map().find(p);
            if (it == map().end())
                return;
            dropped = std::move(it->second);
            map().erase(it);
        }
    }

    // The strong handle of a tracked object, or a null handle.
    static std::shared_ptr<T> find(T* p)
    {
        std::lock_guard<std::mutex> lock(mutex());
        auto it = map().find(p);
        return it == map().end() ? std::shared_ptr<T>() : it->second;
    }

private:
    static std::mutex& mutex()
    {
        static std::mutex m;
        return m;
    }
    static std::unordered_map<T*, std::shared_ptr<T>>& map()
    {
        static std::unordered_map<T*, std::shared_ptr<T>> m;
        return m;
    }
};

// The base of weak_ref_traits<T> for a class that the host tracks with weak_registry<T>.
// A raw pointer of an object that the host did not add gives a null handle: the assignment throws.
template <class T> struct registered_weak_ref : shared_weak_ref<T>
{
    static std::shared_ptr<T> from_raw(T* p)
    {
        return weak_registry<std::remove_const_t<T>>::find(p);
    }
};

} // namespace hot
