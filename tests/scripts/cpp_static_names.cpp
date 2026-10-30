// A script class with static members that have the names of generated functions.
// A static member has the name "<Class>__s_<name>", so it does not collide with "<Class>__plan".
#include "Actor.h"

class Names : public Actor
{
public:
    Names() : Actor("Names") {}

    void Tick(float dt) override
    {
        last = plan() + super_args() + new_default() + construct_keys;
        Move(Vec3(dt, 0, 0));
    }

    static int plan() { return 1; }
    static int super_args() { return 20; }
    static int new_default() { return 300; }
    static inline const int construct_keys = 4000;

    int last = 0;
};
