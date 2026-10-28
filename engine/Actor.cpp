// Out-of-line definitions keep the vtable and the type_info of Actor in the
// executable only. A shared library then uses the executable symbols.
#include "Actor.h"

#include <cstdio>

Actor::Actor() : Name("Actor") {}
Actor::Actor(std::string name) : Name(std::move(name)) {}
Actor::~Actor() = default;

void Actor::Tick(float dt)
{
    std::printf("[engine] %s: Actor::Tick dt=%.4f\n", Name.c_str(), dt);
}

void Actor::TakeDamage(float amount)
{
    std::printf("[engine] %s: Actor::TakeDamage %.1f\n", Name.c_str(), amount);
}

void Actor::TakeDamage(Actor& source, float amount)
{
    std::printf("[engine] %s: Actor::TakeDamage %.1f from %s\n", Name.c_str(), amount,
                source.Name.c_str());
}

void Actor::Move(Vec3 delta)
{
    Position.x += delta.x;
    Position.y += delta.y;
    Position.z += delta.z;
}

World::~World()
{
    for (Actor* a : actors_) delete a;
}

void World::Add(Actor* actor)
{
    actors_.push_back(actor);
}

void World::Tick(float dt)
{
    for (Actor* a : actors_) a->Tick(dt);
}
