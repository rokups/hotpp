// A plain C++ script with free functions that use a module variable. A normal C++ build
// compiles this file. The tool sppc also translates it. It has no include of the spp headers.
#include "Actor.h"

// A module variable. It keeps its value across a reload.
int total = 0;

// A free function without a module variable. It stays as written.
static int twice(int a)
{
    return a * 2;
}

// Free functions that use the module variable. The default argument is only in the prototype.
static int add_total(int a, int b = 1)
{
    total += twice(a) + b;
    return total;
}

inline int read_total()
{
    return total;
}

class Counter : public Actor
{
public:
    int last = 0;

    Counter() : Actor("Counter") {}

    void Tick(float dt) override
    {
        last = add_total(3);
        total += 100;
        Move(Vec3(dt, 0, 0));
    }

    // A method calls a free function that reads the module variable.
    int Peek() { return read_total(); }
};
