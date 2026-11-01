// Generation 2 of the scripts in namespaces. Two classes have the name Unit in two namespaces.
// The variable game::score has the new type long long. The hook convert_game__score converts the
// old value. Player has a new field. The two classes Unit change in different ways.
// The namespace game has a module variable, a function, a class and a static data member. The
// namespace game::ai has a module variable and a class that derives from a class of game.
// A normal C++ build compiles this file. The tool hotc also translates it.
#include "Actor.h"
#include "EngineMath.h"

namespace red
{
class Unit : public Actor
{
public:
    Unit() : Actor("RedUnit") {}

    void Tick(float) override { hits += 2; }

    int hits = 0;
};
} // namespace red

namespace blue
{
class Unit : public Actor
{
public:
    Unit() : Actor("BlueUnit") {}

    void Tick(float dt) override
    {
        hits += 20;
        Move(Vec3(dt, 0, 0));
    }

    int hits = 0;
    int armor = 5;
    int shield = 9;
};
} // namespace blue

namespace game
{
// A module variable in a namespace. A function of the namespace reads it by its name.
long long score = 0;

int bonus(int x)
{
    return x + static_cast<int>(score);
}

class Player : public Actor
{
public:
    Player() : Actor("Player") {}

    void Tick(float dt) override
    {
        score += 1;
        points = bonus(10);
        Move(Vec3(dt, 0, 0));
    }

    int points = 0;
    int level = 4;
};
} // namespace game

// The hook is at file scope. Its name has the namespace.
long long convert_game__score(int old)
{
    return static_cast<long long>(old) * 100;
}

// The compact form of a nested namespace. Brain derives from Player of the outer namespace.
namespace game::ai
{
int mood = 3;

class Brain : public Player
{
public:
    Brain() : Player() {}

    void Tick(float dt) override
    {
        Player::Tick(dt);
        mood += 2;
        thought = bonus(1) + mood;
    }

    int thought = 0;
};
} // namespace game::ai

namespace gai = game::ai;
using namespace game::ai;

// A function at file scope reads a module variable through a namespace alias.
int mood_now()
{
    return gai::mood;
}

// A module variable at file scope holds a pointer to a class of a namespace.
game::ai::Brain* brain = nullptr;

// A class at file scope uses the classes of the namespaces by a qualified name and by a using
// directive.
class Watcher : public Actor
{
public:
    Watcher() : Actor("Watcher") {}

    void Tick(float dt) override
    {
        if (!brain)
            brain = new game::ai::Brain();
        brain->Tick(dt);
        Brain* same = brain;
        game::Player* parent = same;
        seen = mood_now() + game::bonus(0);
        points_seen = parent->points;
        thought_seen = same->thought;
    }

    int seen = 0;
    int points_seen = 0;
    int thought_seen = 0;
};
