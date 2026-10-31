---
layout: default
title: C++ script example
nav_order: 23
group: C++ scripts
---

This page shows two generations of a `.cpp` script and the CMake commands that build them.

## Example

The file `tests/scripts/cpp_player_v1.cpp` is generation 1.

```cpp
// Generation 1 of the Player class as a plain C++ script.
// A normal C++ build compiles this file. The tool hotc also translates it.
#include "Actor.h"
#include "Math.h"

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
```

The file `tests/scripts/cpp_player_v2.cpp` is generation 2. It makes these changes:

- The field `hp` has the new name `health`. `HOT_ALIAS("hp")` keeps the value.
- The field `level` changes from `int` to `float`. The method `convert_level` converts the value.
- The module variable `total` changes from `int` to `float`. The free function `convert_total` converts the value.
- The method `onReload` clamps `health`.
- The method `Tick` moves twice as fast.

```cpp
// Generation 2 of the Player class as a plain C++ script.
// The field hp has the new name health. The field level changes from int to float.
// The module variable total changes from int to float.
#include "Actor.h"
#include "Math.h"

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

    void onReload(const hot::OldState& old)
    {
        if (old.has("hp") && old.get<float>("hp") > 100.0f)
            health = 100.0f;
    }

    HOT_ALIAS("hp") float health = 100.0f;
    float speed = 5.0f;
    std::string target;
    Point p{1, 2};
    Mode mode = Mode::Idle;
    float level = 1.0f;
};
```

## CMake

`hot_add_scripts` takes `.cpp` paths in `SCRIPTS`, as it takes `.hot` paths. A `foo.hot` and a `foo.cpp` in one call fail at configure time, because they have the same stem. See [CMake](cmake-scripts.html).

```cmake
hot_add_scripts(TARGET gen1 SCRIPTS cpp_player_v1.cpp)
hot_add_scripts(TARGET gen2 SCRIPTS cpp_player_v2.cpp AFTER gen1 HOTC_ARGS --gen 2)
```
