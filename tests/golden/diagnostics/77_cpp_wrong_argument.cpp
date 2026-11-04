#include "Actor.h"

class Player : public Actor
{
public:
    void Tick(float dt) override
    {
        heal("x");
    }

    void heal(float a)
    {
        hp += a;
    }

    int hp = 10;
};
