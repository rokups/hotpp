// Generation 1 of a script class with a constructor member initializer list.
// A normal C++ build compiles this file. The tool hotc also translates it.
#include "Actor.h"

#include <string>
#include <utility>

class Unit : public Actor
{
public:
    // The list order differs from the field order. C++ uses the field order.
    Unit(int hp_, std::string n, int bonus = 1)
        : Actor("Unit")
        , name(std::move(n))
        , twice(hp + tag)
        , tag{bonus}
        , hp(hp_ * 2)
        , len(static_cast<int>(Name.size()))
    {
        stamp += 1;
    }

    void Tick(float dt) override { Move(Vec3(dt, 0, 0)); }

    int hp;
    int tag;
    std::string name;
    int twice;
    int len;
    int stamp = 5;
    int kept = 7;
};

Unit* first = nullptr;

class Spawner : public Actor
{
public:
    Spawner() : Actor("Spawner") {}
    // The spawner owns the object of the module variable. Without this, it stays alive after the test.
    ~Spawner()
    {
        delete first;
        first = nullptr;
    }

    void Tick(float) override
    {
        if (!first)
            first = new Unit(4, "bob");
    }
};
