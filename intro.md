---
layout: default
title: Introduction
nav_order: 1
---

Hot++ is a scripting layer for C++ programs. You write a class in a script. The program loads the script and runs it as native code.

When you save a changed script, the running program reloads it. Live objects keep their field values. A script class can derive from a native C++ class and call native members with no wrapper code.

The tool `hotc` turns a script into C++. A program can use the result in three ways: ahead of time (AOT), compiled at run time (JIT), or from files that the engine watches.

A script is one of two kinds. A `.hot` script has a C#-like syntax. A `.cpp` script is a plain C++ file.

## The same class in both forms

This `.hot` script is a `Player` that derives from the native class `Actor`.

```hot
using "Actor.h";
using "Math.h";

int ticks = 0;

class Player : Actor {
    float hp = 100.0;
    float speed = 5.0;

    override void Tick(float dt) {
        Move(Vec3(speed * dt, 0, 0));
        heal(1.0);
        ticks++;
    }

    void heal(float amount) {
        hp = Math.Min(hp + amount, 100.0);
    }
}
```

This `.cpp` script does the same. A normal C++ build compiles it with no `hotc`. `hotc` also translates it, and the program reloads it in the same way.

```cpp
#include "Actor.h"
#include "Math.h"

int ticks = 0;

class Player : public Actor
{
public:
    void Tick(float dt) override
    {
        Move(Vec3(speed * dt, 0, 0));
        heal(1.0f);
        ticks++;
    }

    void heal(float amount)
    {
        hp = Math::Min(hp + amount, 100.0f);
    }

private:
    float hp = 100.0f;
    float speed = 5.0f;
};
```

See [Getting started](getting-started.html) for the `.hot` form and [C++ scripts](cpp-scripts.html) for the `.cpp` form.

## How it works

`hotc` gives both scripts the same C++. The code below is a small, hand-written and simplified version of that output. The real output has more code: line markers, checks, constructors and the other `Actor` virtual methods. Names are close to the real names.

```cpp
// 1. Module block. The variables outside classes live here.
struct HotModule { int ticks; };
HotModule* hot_mod();                // the loader points it at the current block

// 2. State of generation 1. The fields and the script code live here.
struct PlayerState_g1 {
    hot::ShellBase* owner;           // the shell that this state belongs to
    float hp, speed;
    void Tick(float dt) {
        owner->Move(Vec3(speed * dt, 0, 0));   // a native call, as written
        heal(1.0f);
        hot_mod()->ticks++;                    // "ticks" becomes a module field
    }
    void heal(float amount) { hp = Math::Min(hp + amount, 100.0f); }
};

// 3. Shell. The engine sees this object. It never changes its address.
enum { SLOT_Tick, SLOT_heal };
struct Player_Shell final : Actor, hot::ShellBase {
    void Tick(float dt) override { hot::call<void(Actor*, float)>(this, SLOT_Tick, dt); }
};

// 4. Slot table of generation 1. A reload makes a new table.
static const hot::SlotFn slots[] = {
    (hot::SlotFn)&PlayerState_g1::Tick,
    (hot::SlotFn)&PlayerState_g1::heal,
};

// 5. Field list. The loader uses it to move the values to a new generation.
static const hot::FieldInfo fields[] = {
    {"hp",    typeid(float).name(), offsetof(PlayerState_g1, hp),    sizeof(float)},
    {"speed", typeid(float).name(), offsetof(PlayerState_g1, speed), sizeof(float)},
};

// 6. Entry point. The loader calls it when it loads the library.
extern "C" void script_register(hot::Registry& reg) { reg.add(&Player_record, &Player_create); }
```

This design gives the features above:

- **A reload keeps state.** The fields are in `PlayerState_gN`, not in the shell. A new generation has a new state struct. The loader moves each field from the old state to the new state when the name and the type are the same. A new field gets its default value.
- **The code changes, the object does not.** The shell keeps its address. The engine keeps its pointer. A call goes through the slot table, so it reaches the code of the newest generation.
- **Native base classes work.** The shell derives from `Actor`. The engine sees a real `Actor`. A call to `Move` in the script is a plain C++ call.
- **Module variables survive too.** The block `hot_mod()` moves between generations in the same way as the fields.
- **Old code is safe.** Each generation has a live flag. A call into a retired generation does not run its code. The loader unloads the old library only when no object or call uses it.
- **Two kinds, one result.** A `.hot` script and a `.cpp` script go through the same output, so they reload in the same way.

To read the real output, run `hotc --emit-cpp player.hot --gen 1 -I include -I tests/engine`.
