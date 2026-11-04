#include "Actor.h"

class Player : public Actor
{
public:
    void Tick(float dt) override
    {
        hpp = 0;
        this->Jump();
    }

    int hp = 10;
};
