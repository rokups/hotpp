// Generation 2 of a script class with a constructor member initializer list.
// The initializers change. The class has a new field. The spawner makes a second object.
// A normal C++ build compiles this file. The tool hotc also translates it.
#include "Actor.h"

#include <string>
#include <utility>

class Unit : public Actor
{
public:
    Unit(int hp_, std::string n, int bonus = 1)
        : Actor("Unit")
        , name(std::move(n) + "!")
        , twice(hp + tag)
        , tag{bonus * 10}
        , hp(hp_ * 3)
        , len(static_cast<int>(Name.size()))
        , extra(hp_)
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
    int extra;
};

Unit* first = nullptr;
Unit* second = nullptr;

class Spawner : public Actor
{
public:
    Spawner() : Actor("Spawner") {}

    void Tick(float) override
    {
        if (!first)
            first = new Unit(4, "bob");
        if (!second)
            second = new Unit(4, "bob");
    }
};
