// A plain C++ script with two warnings. The tests compile it with -Wall -Werror and check that the
// diagnostics show the script text.
#include "Actor.h"

class Player : public Actor
{
public:
    void Tick(float dt) override
    {
        int unused = 1;
        if (hp = dt)
            hp = 0;
        auto f = [this] { return hp; };
        f();
    }

    int hp = 10;
};
