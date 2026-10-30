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
