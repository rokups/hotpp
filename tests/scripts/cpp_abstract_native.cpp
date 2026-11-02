// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// A script class that leaves a pure virtual method of a native base open.
// Warden does not override Seal of IStrict, so Warden is abstract. Jailer implements Seal.
// A normal C++ build compiles this file. The tool hotc also translates it.
#include "Actor.h"
#include "Guarded.h"

class Warden : public Actor, public IStrict
{
public:
    Warden() : Actor("Warden") {}

    int sealed = 0;
};

class Jailer : public Warden
{
public:
    int Seal(int x) noexcept override { return x + 1; }
};
