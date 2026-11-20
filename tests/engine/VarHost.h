// A native value type and a native host that stores a pointer to it, for the tests of a field
// that goes to a native "T*" parameter. The shapes follow Urho3D::Variant and RmlUIComponent.
#pragma once
#include "Actor.h"

namespace Urho3D
{

class Variant
{
public:
    Variant() = default;
    Variant(int v)
        : value_(v)
    {
    }
    Variant(const Variant&) = default;
    Variant& operator=(const Variant&) = default;
    int GetInt() const { return value_; }

private:
    int value_ = 0;
};

class VarHost : public ::Actor
{
public:
    // The host keeps the pointer, as RmlUIComponent::BindDataModelVariant does.
    void BindDataModelVariant(const std::string&, Variant* p) { bound_ = p; }
    Variant* Bound() const { return bound_; }
    // A "void*" parameter and a template parameter keep the pointer too.
    void KeepRaw(void* p) { raw_ = p; }
    template <class P> void KeepAny(P p) { any_ = static_cast<void*>(p); }
    void* Raw() const { return raw_; }
    void* Any() const { return any_; }

private:
    Variant* bound_ = nullptr;
    void* raw_ = nullptr;
    void* any_ = nullptr;
};

} // namespace Urho3D
