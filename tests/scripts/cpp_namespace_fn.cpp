// A plain C++ script with a function in a namespace that uses a module variable. A normal C++
// build compiles this file. The tool sppc also translates it. It has no include of the spp headers.
#include "Actor.h"

// A module variable. It keeps its value across a reload.
int total = 0;

namespace counter
{
// A function in a namespace that uses the module variable. It stays in its place in the output.
int bump(int n)
{
    total += n;
    return total;
}
} // namespace counter

class Bumper : public Actor
{
public:
    int last = 0;

    Bumper() : Actor("Bumper") {}

    void Tick(float dt) override
    {
        last = counter::bump(5);
        total += 100;
        Move(Vec3(dt, 0, 0));
    }
};
