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
#include "hot/runtime/export.h"

#include <memory>
#include <type_traits>
#include <typeinfo>

namespace hot
{

namespace detail
{
// The storage lives in the host (src/runtime/weak_registry.cpp). A module has its own copy of
// each inline function and each static local, so a map in this header would be empty in a module.
// The key is a type name: the type_info address of one type can differ between two images.
HOT_EXPORT void weak_registry_add(const char* type_key, void* p, std::shared_ptr<void> keep);
HOT_EXPORT void weak_registry_remove(const char* type_key, void* p);
HOT_EXPORT std::shared_ptr<void> weak_registry_find(const char* type_key, void* p);
} // namespace detail

template <class T> class weak_registry
{
public:
    // Starts to track the object. A second call for the same object changes nothing.
    static void add(T* p)
    {
        if (!p)
            return;
        // The owner is the registry. The deleter does nothing: the host owns the object.
        detail::weak_registry_add(key(), const_cast<std::remove_const_t<T>*>(p),
                                  std::shared_ptr<T>(p, [](T*) {}));
    }

    // Stops to track the object. Each weak pointer to it reads null from now on.
    static void remove(T* p) { detail::weak_registry_remove(key(), const_cast<std::remove_const_t<T>*>(p)); }

    // The strong handle of a tracked object, or a null handle.
    static std::shared_ptr<T> find(T* p)
    {
        return std::static_pointer_cast<T>(
            detail::weak_registry_find(key(), const_cast<std::remove_const_t<T>*>(p)));
    }

private:
    // A pointer type is always complete, so T can be an opaque class.
    static const char* key() { return typeid(T*).name(); }
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
