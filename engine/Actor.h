// Engine API for the minengine harness (design section 8.1).
// Script code calls these members directly. There is no wrapper code.
#pragma once
#include <string>
#include <vector>

struct Vec3 {
    float x = 0, y = 0, z = 0;
};

class Actor {
public:
    Actor();
    explicit Actor(std::string name);
    virtual ~Actor();

    virtual void Tick(float dt);
    virtual void TakeDamage(float amount);
    virtual void TakeDamage(Actor& source, float amount);

    void Move(Vec3 delta);

    std::string Name;
    Vec3 Position;
};

// The world owns the actors that it holds. It deletes them in the destructor.
class World {
public:
    World() = default;
    World(const World&) = delete;
    World& operator=(const World&) = delete;
    ~World();

    void Add(Actor* actor);
    void Tick(float dt);
    const std::vector<Actor*>& Actors() const { return actors_; }

private:
    std::vector<Actor*> actors_;
};
