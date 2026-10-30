// A plain C++ script with default arguments. A normal C++ build compiles this file.
// The tool hotc also translates it. It has no include of the hot headers.
#include "Actor.h"

const int kStep = 2;

// A module variable. A default argument can read it.
int calls = 0;

class Counter : public Actor
{
public:
    int value = 0;
    int last = 0;

    // A constructor with a default argument. The host creates the object without arguments.
    Counter(int start = kStep * 3) : Actor("Counter")
    {
        value = start;
    }

    // The default of "b" is a constant. The default of "c" is a module variable.
    int Add(int a, int b = kStep, int c = calls)
    {
        calls++;
        return a + b + c;
    }

    void Tick(float dt) override
    {
        value = Add(10);
        last = Add(10, 5, 100);
        Move(Vec3(dt, 0, 0));
    }

    // An override has no default argument: the call through the base sees the base declaration.
    void TakeDamage(float amount) override
    {
        value -= static_cast<int>(amount);
    }
};
