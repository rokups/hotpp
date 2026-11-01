// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Generation 1: a static data member and a module variable. No convert hook exists.
#include "Actor.h"

int level = 10;

class Gauge : public Actor
{
public:
    Gauge() : Actor("Gauge") {}

    void Tick(float dt) override
    {
        ++reading;
        ++level;
        Move(Vec3(dt, 0, 0));
    }

    static int reading;
};

int Gauge::reading = 10;
