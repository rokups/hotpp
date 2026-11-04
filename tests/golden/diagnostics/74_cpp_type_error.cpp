#include "Actor.h"

#include <string>

class Player : public Actor
{
public:
    void Tick(float dt) override
    {
        std::string s = "text";
        int n = s;
        hp = n;
    }

    int hp = 10;
};
