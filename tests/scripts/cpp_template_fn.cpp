// A plain C++ script with a function template that uses a module variable. A normal C++ build
// compiles this file. The tool hotc also translates it. It has no include of the hot headers.
#include "Actor.h"

// A module variable. It keeps its value across a reload.
int total = 0;

// A function template that uses the module variable. It stays in its place in the output.
template <class T>
T scaled(T k)
{
    return k * static_cast<T>(total);
}

class Scaler : public Actor
{
public:
    int last = 0;

    Scaler() : Actor("Scaler") {}

    void Tick(float dt) override
    {
        total += 2;
        last = scaled(3);
        Move(Vec3(dt, 0, 0));
    }
};
