// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Native interfaces for script classes with more than one native base (S8b, design 4.4).
// A script class names them after its primary base: "class Boss : Actor, IDamageable".
// The shell derives from all bases. It has one slot for each virtual of each base.
#pragma once
#include "hot/runtime/export.h" // HOT_RTTI

class HOT_RTTI IDamageable
{
public:
    virtual ~IDamageable() = default;
    // A pure virtual: a script class with this base must override it.
    virtual int Absorb(int amount) = 0;
    // A virtual with an implementation: the shell calls it when the script has no override.
    virtual int Armor() const { return 2; }
};

class HOT_RTTI ITagged
{
public:
    virtual ~ITagged() = default;
    virtual int Tag() { return 7; }
};

// Native code that knows only the interface. It calls the virtuals through the interface.
inline int HitThrough(IDamageable& target, int amount)
{
    return target.Absorb(amount) + target.Armor();
}
