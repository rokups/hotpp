// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Generation 2: both variables change the type. No convert hook exists.
#include "Actor.h"

float level = 4.0f;

class Gauge : public Actor
{
public:
    Gauge() : Actor("Gauge") {}

    void Tick(float dt) override
    {
        reading += 1.0f;
        level += 1.0f;
        Move(Vec3(dt, 0, 0));
    }

    static float reading;
};

float Gauge::reading = 4.0f;
