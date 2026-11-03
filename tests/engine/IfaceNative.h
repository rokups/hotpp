// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// A native base with overloaded virtual methods. The interface tests use it: a script class
// overrides one overload and implements an interface method with it.
#pragma once
#include "Actor.h"

class IfaceNative : public Actor
{
public:
    virtual int Foo(int x) { return x; }
    virtual int Foo(float x) { return 1000 + static_cast<int>(x); }
    virtual int Area() { return 7; }
};
