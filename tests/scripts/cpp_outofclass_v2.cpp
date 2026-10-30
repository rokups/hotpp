// Generation 2 of the Fighter class. The class declares the methods. The definitions come after
// another class, outside the class. The method hit now doubles the damage.
#include "Actor.h"

int ticks = 0;
int step = 1;

class Fighter : public Actor
{
public:
    Fighter();
    ~Fighter();

    void Tick(float dt) override;
    // The default argument is only in the class.
    int hit(int d, int scale = step);
    int get() const;

    int hp = 20;
    int last = 0;
};

class Marker : public Actor
{
public:
    Marker() : Actor("Marker") {}
    int n = 0;
};

Fighter::Fighter() : Actor("Fighter")
{
    ticks += 100;
}

Fighter::~Fighter()
{
    ticks = -1;
}

void Fighter::Tick(float dt)
{
    Move(Vec3(2.0f * dt, 0, 0));
    ticks++;
    hit(1);
    last = get();
}

// The parameter names of the definition are the names of the body.
int Fighter::hit(int damage, int factor)
{
    hp -= 2 * damage * factor;
    return this->hp;
}

int Fighter::get() const
{
    return hp;
}
