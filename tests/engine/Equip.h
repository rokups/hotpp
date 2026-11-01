// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Native classes with virtual methods that take a handle type in several forms: a raw
// pointer, a const reference, a std::shared_ptr and an intrusive pointer. A script override
// writes the plain handle type for each form. The functions call the virtuals as native code.
#pragma once
#include "Object.h"

#include <memory>

struct Charm : std::enable_shared_from_this<Charm>
{
    int power = 0;
};

// A class that gets shared_from_this from its base Charm.
struct Blade : Charm
{
};

// No shared_from_this base: only the exact native types work for it.
struct Rune
{
    int power = 0;
};

class Wearer
{
public:
    virtual ~Wearer() = default;
    virtual int WearRaw([[maybe_unused]] Charm* c) { return 0; }
    virtual int WearRef([[maybe_unused]] const Charm& c) { return 0; }
    virtual int WearShared([[maybe_unused]] std::shared_ptr<Charm> c) { return 0; }
    virtual int WearItem([[maybe_unused]] Item* i) { return 0; }
    virtual int WearBlade([[maybe_unused]] Blade* b) { return 0; }
    // Returns: only the handle by value can be overridden with the plain handle type.
    virtual std::shared_ptr<Charm> Spawn() { return nullptr; }
    virtual std::shared_ptr<Charm> Make([[maybe_unused]] int power) { return nullptr; }
    virtual Charm* Borrow() { return nullptr; }
};

inline int WearThrough(Wearer& w, const std::shared_ptr<Charm>& c, int which)
{
    switch (which)
    {
    case 0: return w.WearRaw(c.get());
    case 1: return w.WearRef(*c);
    default: return w.WearShared(c);
    }
}

inline int WearItemThrough(Wearer& w, Item* i)
{
    return w.WearItem(i);
}

// Native functions that take a handle type in one form (a script passes the plain handle) and
// in two forms (the script must write the form: no conversion). The null pointer gives -1.
inline int Polish(Charm* c)
{
    return c ? c->power : -1;
}

inline int Shine(const Charm& c)
{
    return c.power;
}

inline int Mixed(Charm* c)
{
    return c ? 1 : -1;
}

inline int Mixed(const Charm& c)
{
    return c.power >= 0 ? 2 : -2;
}
