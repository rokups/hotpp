// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// A mock host type system. Each typed class has one static TypeTag. An object reports the tag of
// its class through a virtual. A prelude rule puts HOT_TEST_TYPED in the shell of each script class.
#pragma once
#include "Actor.h"

struct TypeTag
{
    const char* name;
    const TypeTag* base;
};

class TypedActor : public Actor
{
public:
    TypedActor()
        : Actor("typed")
    {
    }
    virtual const TypeTag* GetTag() const { return GetTagStatic(); }
    static const TypeTag* GetTagStatic()
    {
        static const TypeTag tag{"TypedActor", nullptr};
        return &tag;
    }
};

// The class name "type" must be a C++ type. The base "base" has GetTagStatic.
#define HOT_TEST_TYPED(type, base)                                                                                     \
public:                                                                                                                \
    using ClassName = type;                                                                                            \
    const TypeTag* GetTag() const override { return GetTagStatic(); }                                                  \
    static const TypeTag* GetTagStatic()                                                                               \
    {                                                                                                                  \
        static const TypeTag tag{#type, base::GetTagStatic()};                                                         \
        return &tag;                                                                                                   \
    }
