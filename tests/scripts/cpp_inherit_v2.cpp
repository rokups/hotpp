// Generation 2 of the Base and Kid classes: a script class derives from a script class.
// Base has the new field armor. Kid has the new field gen, a new override of the native virtual
// TakeDamage and the new virtual method Bonus. Power of Kid gives 7.
// A normal C++ build compiles this file. The tool hotc also translates it.
#include "Actor.h"
#include "Math.h"

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
    int armor = 4;
};

// The last object that ran Tick, and the number of casts to Kid that found the object.
Base* last_ticked = nullptr;
int kid_casts = 0;

class Kid : public Base
{
public:
    // The argument goes to the constructor of Base.
    Kid(int start = 3) : Base(start * 2) { kid_hits = start; }

    int Power() override { return 7; }

    // A new override in this generation. The override of Base runs with three times the amount.
    void TakeDamage(float amount) override
    {
        Base::TakeDamage(amount * 3.0f);
        this->kid_hits += 1;
    }

    // A new virtual method in this generation.
    virtual int Bonus() { return gen * 10; }

    void Tick(float dt) override
    {
        Base::Tick(dt);
        Move(Vec3(1.0f, 0, 0));
        moved = Position.x;
        this->Bump();
        twice = Twice();
        super_power = this->Base::Power();
        bonus = Bonus();
        last_ticked = this;
        Kid* k = dynamic_cast<Kid*>(last_ticked);
        if (k == this)
            kid_casts++;
        seen_hits = last_ticked->hits + k->kid_hits + k->Bonus();
    }

    int kid_hits = 0;
    float moved = 0.0f;
    int super_power = 0;
    int seen_hits = 0;
    int gen = 2;
    int bonus = 0;
};
