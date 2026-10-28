// The view of the old state block that the onReload hook gets (design 4.7 and 7.8).
#pragma once
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <typeinfo>

#include "spp/runtime/records.h"

namespace spp {

// A read-only view of the old block of one object. It shows each field of the old
// generation by its old name, also a field that the migration moved. The old block
// stays valid until the hooks of all objects are done.
class OldState {
public:
    OldState(const StateLayout* layout, const char* block) : layout_(layout), block_(block) {}

    // True when the old generation had a field with this name.
    bool has(std::string_view name) const { return find(name) != nullptr; }

    // True when the old field exists and its type is T.
    template <class T>
    bool has(std::string_view name) const
    {
        const FieldInfo* f = find(name);
        return f && std::strcmp(f->type_key, typeid(T).name()) == 0;
    }

    // The old value. It throws std::runtime_error when the field is absent or has
    // another type than T. The hook can catch it, or the runtime logs it.
    template <class T>
    const T& get(std::string_view name) const
    {
        const FieldInfo* f = find(name);
        if (!f) throw std::runtime_error("OldState: no old field '" + std::string(name) + "'");
        if (std::strcmp(f->type_key, typeid(T).name()) != 0)
            throw std::runtime_error("OldState: old field '" + std::string(name) +
                                     "' has another type");
        return *reinterpret_cast<const T*>(block_ + f->offset);
    }

private:
    const FieldInfo* find(std::string_view name) const
    {
        for (uint32_t i = 0; i < layout_->field_count; ++i)
            if (name == layout_->fields[i].name) return &layout_->fields[i];
        return nullptr;
    }
    const StateLayout* layout_;
    const char* block_;
};

}  // namespace spp
