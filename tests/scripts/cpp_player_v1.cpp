// Generation 1 of the Player class as a plain C++ script.
// A normal C++ build compiles this file. The tool sppc also translates it.
#include "Actor.h"
#include "Math.h"
#include "spp/script.h"

#include <cstdio>
#include <string>

enum class Mode { Idle, Run };

struct Point
{
    float x = 0, y = 0;
};

// A module variable. It keeps its value across a reload.
int total = 0;
const float kMaxHp = 100.0f;

static float clamp_hp(float v)
{
    return Math::Max(Math::Min(v, kMaxHp), 0.0f);
}

class Player : public Actor
{
public:
    Player() : Actor("Player") {}

    void Tick(float dt) override
    {
        Move(Vec3(speed * dt, 0, 0));
        std::printf("[v1] %s: Tick dt=%.4f pos.x=%.4f hp=%.1f\n", this->Name.c_str(), dt, Position.x, hp);
        heal(1.0f);
        total++;
        auto step = [&] { Move(Vec3(0, 1, 0)); };
        step();
    }

    void TakeDamage(float amount) override
    {
        hp = clamp_hp(hp - amount);
        std::printf("[v1] %s: TakeDamage %.1f -> hp=%.1f\n", this->Name.c_str(), amount, hp);
    }

    void TakeDamage(Actor& source, float amount) override
    {
        target = source.Name;
        std::printf("[v1] %s: hit by %s\n", this->Name.c_str(), target.c_str());
        TakeDamage(amount);
    }

    void heal(float a)
    {
        hp = clamp_hp(hp + a);
        std::printf("[v1] %s: heal %.1f -> hp=%.1f\n", this->Name.c_str(), a, hp);
    }

    float hp = 100.0f;
    float speed = 5.0f;
    std::string target;
    Point p{1, 2};
    Mode mode = Mode::Idle;
    int level = 1;
};
