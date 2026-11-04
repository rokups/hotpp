#include "Actor.h"

class Player : public Actor
{
public:
    using Score = int;

    void Tick(float dt) override
    {
        hp = 0;
    }

    int hp = 10;
};
