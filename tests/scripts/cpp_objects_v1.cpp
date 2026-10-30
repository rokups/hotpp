// Generation 1 of two script classes. Script code uses a script class as a type.
// A normal C++ build compiles this file. The tool sppc also translates it.
#include "Actor.h"

class Minion : public Actor
{
public:
    Minion(int p = 1) : Actor("Minion") { power = p; }

    void Tick(float dt) override { Move(Vec3(dt, 0, 0)); }

    void boost(int v) { power += v; }
    int total() const { return power + hits; }

    // Another object of the same class: a field access and a method call on it.
    void copy_from(const Minion& other) { power = other.power; }

    // A static method and a static data member name their own class before its class block.
    static Minion* spawn(int p) { return new Minion(p); }
    inline static Minion* first_made = nullptr;

    int power = 0;
    int hits = 0;
};

// Module variables keep their values across a reload. "minion" points to a script object.
Minion* minion = nullptr;
int cast_hits = 0;

// A free function uses the script class as a type.
static Minion* find_minion(Actor* a)
{
    return dynamic_cast<Minion*>(a);
}

class Spawner : public Actor
{
public:
    Spawner() : Actor("Spawner") {}

    void Tick(float dt) override
    {
        if (!minion)
            minion = new Minion(3);
        if (!Minion::first_made)
            Minion::first_made = Minion::spawn(1);
        minion->boost(2);
        minion->hits += 1;
        minion->Tick(dt);
        Actor* as_actor = minion;
        if (find_minion(as_actor) == minion && dynamic_cast<Minion*>(static_cast<Actor*>(this)) == nullptr)
            cast_hits++;
        Minion& ref = dynamic_cast<Minion&>(*as_actor);
        last = ref.power + static_cast<Minion*>(as_actor)->total() + static_cast<int>(sizeof(Minion) > 0);
    }

    int last = 0;
};
