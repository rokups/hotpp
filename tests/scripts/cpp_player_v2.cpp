// Generation 2 of the Player class as a plain C++ script.
// The field hp has the new name health. The field level changes from int to float.
// The module variable total changes from int to float.
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

float total = 0;
const float kMaxHp = 100.0f;

static float clamp_hp(float v)
{
    return Math::Max(Math::Min(v, kMaxHp), 0.0f);
}

// The convert hook of the module variable total.
float convert_total(int old)
{
    return old * 2.0f;
}

class Player : public Actor
{
public:
    Player() : Actor("Player") {}

    void Tick(float dt) override
    {
        Move(Vec3(2.0f * speed * dt, 0, 0));
        std::printf("[v2] %s: Tick dt=%.4f pos.x=%.4f health=%.1f level=%.1f\n", this->Name.c_str(), dt,
            Position.x, health, level);
        total++;
        auto step = [&] { Move(Vec3(0, 1, 0)); };
        step();
    }

    void TakeDamage(float amount) override
    {
        health = clamp_hp(health - amount);
        std::printf("[v2] %s: TakeDamage %.1f -> health=%.1f\n", this->Name.c_str(), amount, health);
    }

    void TakeDamage(Actor& source, float amount) override
    {
        target = source.Name;
        std::printf("[v2] %s: hit by %s\n", this->Name.c_str(), target.c_str());
        TakeDamage(amount);
    }

    void heal(float a)
    {
        health = clamp_hp(health + 2.0f * a);
        std::printf("[v2] %s: heal %.1f (x2) -> health=%.1f\n", this->Name.c_str(), a, health);
    }

    // The convert hook of the field level.
    float convert_level(int old) { return old * 10.0f; }

    void onReload(const spp::OldState& old)
    {
        if (old.has("hp") && old.get<float>("hp") > 100.0f)
            health = 100.0f;
    }

    SPP_ALIAS("hp") float health = 100.0f;
    float speed = 5.0f;
    std::string target;
    Point p{1, 2};
    Mode mode = Mode::Idle;
    float level = 1.0f;
};
