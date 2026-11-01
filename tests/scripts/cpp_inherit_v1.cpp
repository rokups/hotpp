// Generation 1 of the Base and Kid classes: a script class derives from a script class.
// Base declares the virtual method Power. Kid inherits Bump and Twice from Base. Kid overrides
// Power and the native virtual Tick. Kid calls the version of the parent with "Base::".
// A normal C++ build compiles this file. The tool hotc also translates it.
#include "Actor.h"
#include "EngineMath.h"

class Base : public Actor
{
public:
    Base(int start = 0) : Actor("Base"), hits(start) {}

    virtual int Power() { return 1; }

    int Twice() { return Power() * 2; }

    void Bump() { hits += 10; }

    void TakeDamage(float amount) override
    {
        hp -= amount;
        hits += 1;
    }

    float hp = 100.0f;
    int hits = 0;
    int twice = 0;
};

// The last object that ran Tick, and the number of casts to Kid that found the object.
Base* last_ticked = nullptr;
int kid_casts = 0;

class Kid : public Base
{
public:
    // The argument goes to the constructor of Base.
    Kid(int start = 3) : Base(start * 2) { kid_hits = start; }

    int Power() override { return 5; }

    void Tick(float dt) override
    {
        Base::Tick(dt);
        Move(Vec3(1.0f, 0, 0));
        moved = Position.x;
        Bump();
        twice = Twice();
        super_power = Base::Power();
        last_ticked = this;
        Kid* k = dynamic_cast<Kid*>(last_ticked);
        if (k == this)
            kid_casts++;
        seen_hits = last_ticked->hits + k->kid_hits;
    }

    int kid_hits = 0;
    float moved = 0.0f;
    int super_power = 0;
    int seen_hits = 0;
};
