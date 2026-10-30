---
layout: default
title: C++ scripts
nav_order: 17
---

A `.cpp` script is a standard C++ file. It uses the same hot reload as a `.spp` script. This page describes the `.cpp` script mode.

## What a `.cpp` script is

A `.cpp` script holds a class that derives from a native engine class. The class has fields and methods. The host gives the path of the file to the engine, as it does for a `.spp` script.

The extension selects the parser:

- `.spp` selects the custom S++ syntax.
- `.cpp` selects libclang.

Both modes work in one program. A `Loader` holds one module for each generation, as for `.spp` scripts. Use one session for each script. There is no import between the modes. A `.cpp` script cannot include a `.spp` script. A `.spp` script cannot import a `.cpp` script.

## The file compiles unchanged

A `.cpp` script is fully standard C++. A normal build can include it. The file includes only its own host headers. It includes no spp header and it uses no spp runtime function.

A normal CMake build compiles the file with no `sppc` and no spp include dir. `sppc` does not inject a header into the file.

There are no hot-reload aids in a `.cpp` script. There is no alias and no `onReload`. The functions `convert_<field>` and `convert_<var>` stay, because they are plain C++.

In a normal build the file has plain C++ meaning. There is no state migration and there are no slots. Do not share objects between the two builds.

## Example

The file `tests/scripts/cpp_player_v1.cpp` is generation 1.

```cpp
// Generation 1 of the Player class as a plain C++ script.
// A normal C++ build compiles this file. The tool sppc also translates it.
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

- The field `level` changes from `int` to `float`. The method `convert_level` converts the value.
- The module variable `total` changes from `int` to `float`. The free function `convert_total` converts the value.
- The field `hp` keeps its name. A `.cpp` script has no alias.
- The method `Tick` moves twice as fast.

```cpp
// Generation 2 of the Player class as a plain C++ script.
// The field level changes from int to float. The module variable total changes from int to float.
// The file is standard C++ and includes only its host headers.
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
        std::printf("[v2] %s: Tick dt=%.4f pos.x=%.4f hp=%.1f level=%.1f\n", this->Name.c_str(), dt,
            Position.x, hp, level);
        total++;
        auto step = [&] { Move(Vec3(0, 1, 0)); };
        step();
    }

    void TakeDamage(float amount) override
    {
        hp = clamp_hp(hp - amount);
        std::printf("[v2] %s: TakeDamage %.1f -> hp=%.1f\n", this->Name.c_str(), amount, hp);
    }

    void TakeDamage(Actor& source, float amount) override
    {
        target = source.Name;
        std::printf("[v2] %s: hit by %s\n", this->Name.c_str(), target.c_str());
        TakeDamage(amount);
    }

    void heal(float a)
    {
        hp = clamp_hp(hp + 2.0f * a);
        std::printf("[v2] %s: heal %.1f (x2) -> hp=%.1f\n", this->Name.c_str(), a, hp);
    }

    // The convert hook of the field level.
    float convert_level(int old) { return old * 10.0f; }

    float hp = 100.0f;
    float speed = 5.0f;
    std::string target;
    Point p{1, 2};
    Mode mode = Mode::Idle;
    float level = 1.0f;
};
```

## How sppc edits a body

`sppc` parses the `.cpp` file with libclang. It keeps each method body as text. It edits only the names that the generated code cannot resolve:

- A bare use or a `this->` use of a native base member goes to the native owner. Examples are `Move(...)` and `this->Name`.
- `this` as an argument becomes the native owner.
- A non-const file-scope variable becomes a migrated module variable.
- A static member or a nested type of the base gets the `Base::` prefix.

All other text stays as you wrote it.

## What a script class can have

- A class that derives from a native class.
- Fields with initializers and migration.
- `convert_<field>` methods.
- Constructors with a base initializer, for example `Actor(args)`.
- Destructors.
- `override` of native virtual methods, and native interface bases.

At file scope a `.cpp` script can have:

- Module variables, with free-function `convert_<var>` hooks.
- Verbatim code: enums, structs without a base, free functions, `using`, namespaces and `const` or `constexpr` variables.

## Convert hooks

These features follow the rules of [Hot reload](hot-reload.html). Only the spelling is C++.

- `convert_<field>(Old old)` is a method of the class. It converts the old value to the new type of the field.
- `convert_<var>(Old old)` is a free function. It converts the old value of a module variable.

A `.cpp` script has no alias and no `onReload`. Use a `.spp` script when you need an alias or an `onReload` hook. A field rename in a `.cpp` script is a field removal plus an add. The value is lost.

## Unsupported forms

A class with no base is not a script class. `sppc` copies it unchanged, like a struct without a base. It has no hot reload.

The first version gives an error for each form in this list. The text in the second column is the message of `sppc`.

| Form | Message |
|---|---|
| A class that derives from another class of the same file | `a .cpp script class cannot derive from a class of the same file (first version)` |
| `virtual` without `override` | `'virtual' is not supported in a .cpp script; use 'override' of a native virtual` |
| A static data member | `a static data member is not supported in a .cpp script` |
| A static member function | `a static method is not supported in a .cpp script` |
| A method defined outside the class | `define the method inside the class` |
| A member initializer in a constructor | `use a default member initializer` |
| A default argument | `a default argument is not supported in a .cpp script` |
| Any other declaration in the class | `this declaration is not supported in a .cpp script class` |
| Properties | There is no property syntax in C++. Use two methods. |
| Script-class handles and `new` | A script class is not a handle in a `.cpp` script. |
| Script inheritance | See "A class that derives from another class of the same file". |
| The name of the script class outside its body | `the script class '<name>' cannot be named outside its body (first version)` |
| A `[this]` lambda that uses a base member | `a lambda that uses a base member must capture with [&] or [=]` |
| A free function that uses a module variable | `a free function cannot use the module variable '<name>' in a .cpp script (first version)` |
| A base member hidden by a macro | `a macro hides a member of the base class; write the member in the script text` |
| Alias and `onReload` | Not available in `.cpp` mode. Use a `.spp` script. |
| An include of a `.spp` script | `a .cpp script cannot include a .spp script` |

For a `[this]` lambda, use `[&]` or `[=]`. For a constructor member initializer, use a default member initializer in the class body.

## Known limits

- An in-class brace initializer of a class-type field is not visited by libclang. A module variable inside such an initializer is not rewritten.
- A `.cpp` translation does one more libclang parse than a `.spp` translation. A reload is slower. One parse of the engine headers takes about 400 ms.
- In a normal build the file has plain C++ meaning. There is no state migration and there are no slots. Objects must not be shared between the two builds.
- The language server and the VS Code extension serve `.spp` only. See [Tools](tools.html).
- The errors of `sppc` for a `.cpp` script appear at build time.

## CMake

`spp_add_scripts` takes `.cpp` paths in `SCRIPTS`, as it takes `.spp` paths. A `foo.spp` and a `foo.cpp` in one call fail at configure time, because they have the same stem. See [CMake](cmake.html).

```cmake
spp_add_scripts(TARGET gen1 SCRIPTS cpp_player_v1.cpp)
spp_add_scripts(TARGET gen2 SCRIPTS cpp_player_v2.cpp AFTER gen1 SPPC_ARGS --gen 2)
```
