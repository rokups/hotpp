// A plain C++ script with a constexpr function that reads a module variable. A normal C++ build
// compiles this file. The tool sppc also translates it. It has no include of the spp headers.
#include "Actor.h"

// A module variable. It keeps its value across a reload.
int total = 0;

// A constant call takes the first branch. Only a call at runtime reads the module variable.
constexpr int limit(bool fixed)
{
    return fixed ? 10 : total;
}

static_assert(limit(true) == 10, "a constant call does not read the module variable");

class Limiter : public Actor
{
public:
    int last = 0;

    Limiter() : Actor("Limiter") {}

    void Tick(float dt) override
    {
        total += 1;
        last = limit(false) + limit(true);
        Move(Vec3(dt, 0, 0));
    }
};
