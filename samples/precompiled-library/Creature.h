// The host class of the sample. The scripts wolf.hot and wolf.cpp derive from it.
// The class is header-only, so the script and the program use the same header.
#pragma once
#include <string>
#include <utility>

class Creature
{
public:
    explicit Creature(std::string name)
        : Name(std::move(name))
    {
    }
    virtual ~Creature() = default;

    // The program calls Tick for each frame. A script class overrides it.
    virtual void Tick(float dt) { (void)dt; }

    std::string Name;
    int Hp = 100;
};
