// Generation 1 of a script class whose methods have their bodies in the class.
// A normal C++ build compiles this file. The tool sppc also translates it.
#include "Actor.h"

int ticks = 0;
int step = 1;

class Fighter : public Actor
{
public:
    Fighter() : Actor("Fighter") {}

    void Tick(float dt) override
    {
        Move(Vec3(dt, 0, 0));
        ticks++;
        hit(1);
        last = get();
    }

    int hit(int d, int scale = step)
    {
        hp -= d * scale;
        return hp;
    }

    int get() const { return hp; }

    int hp = 20;
    int last = 0;
};
