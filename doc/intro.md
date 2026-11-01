---
layout: default
title: Introduction
nav_order: 1
---

Hot++ is a scripting layer for C++ programs. You write a class in a script. The program compiles the script to native code and loads it. When you save the script, the running program reloads it. Live objects keep their state.

A script can derive from a native C++ class. It calls the C++ API directly. There are no bindings and no wrapper code.

## Use cases

- Rapid prototyping: change the code and see the result in the running program.
- Run-time code reload with state transfer: objects stay alive, and their field values move to the new code.
- Scripting without bindings: a script has full access to the C++ API of the host.
- Games: the target is a game or an engine with a frame loop and native classes.

## Example

A `.hot` script. `Wolf` derives from the host class `Creature` (see [samples/aot](https://github.com/rokups/hotpp/tree/master/samples/aot/)):

```hot
using "Creature.h";
using <string>;

class Wolf : Creature {
    int bites = 0;

    override void Tick(float dt) {
        bites = bites + 1;
        Hp = Hp - 1;
        std.string line = "${Name}: bites=${bites} hp=${Hp:03d}";
        std.printf("[wolf] %s\n", line.c_str());
    }
}
```

The host loads generation 1, makes an object and reloads (trimmed from [samples/hot-reload](https://github.com/rokups/hotpp/tree/master/samples/hot-reload/)):

```cpp
hot::Loader loader;
auto backend = hot::make_default_backend(options);      // JIT or clang files
loader.load(std::move(*backend->load({path, text_v1, {}}, 1).module));
std::unique_ptr<Creature> wolf(static_cast<Creature*>(loader.find("Wolf")->create().native));
wolf->Tick(1.0f / 60.0f);

loader.request_reload(backend->load({path, text_v2, {}}, 2));   // validate
loader.commit_pending();                                        // swap, at a safe point
wolf->Tick(1.0f / 60.0f);                                       // same pointer, new code
```

## How it works

1. Parse: `lexy` reads a `.hot` script. `libclang` reads a `.cpp` script.
2. Check: `libclang` reads the native headers. Hot++ finds the base classes, the virtual methods and the types.
3. Translate to C++: `hotc` writes a state struct, a shell class that derives from the native base, and a slot table.
4. Compile: Clang compiles the C++ in the process (JIT), through `clang++` (clang files), or ahead of time (AOT).
5. Load and swap: the loader moves the fields to the new generation and swaps the slot table. The shell keeps its address.

See [The generated C++](#the-generated-c) for a sketch of the code. What Hot++ uses:

- [lexy](https://github.com/foonathan/lexy): the parser of the `.hot` syntax.
- `libclang` (C API): reads the native headers and the `.cpp` scripts.
- Clang: compiles the generated C++, as a library (JIT) or as `clang++` (files).
- LLVM ORC (`LLJIT`): loads the compiled code in the process and unloads it.
- [fmt](https://github.com/fmtlib/fmt): string interpolation `${}` in C++17. C++20 can use `std::format`.
- [nlohmann_json](https://github.com/nlohmann/json) and `clangd`: the language server `hot-ls`.
- CMake with `CPM.cmake`: the build, and the fetch of missing packages. GoogleTest runs the tests.

## Three forms of a script

- `.hot`: a C#-like syntax with handles, properties, `foreach`, lambdas and string interpolation.
- `.cpp`: a plain C++ file. A normal build compiles it, and `hotc` translates it for reload.
- Native C++: the host code, and the reference for speed.

The numbers come from `bench_backends` (Release build, median of 3 runs). See [Benchmarks](benchmarks.html).

- A loop inside a script runs at native speed (0.94x to 1.0x), for `.hot` and for `.cpp`.
- A call from the host into a script costs 2.2x to 2.4x of a native virtual call in `files` and `aot` (about 2 ns more). In `jit` it costs 2.9x.
- `new` and free of a `.hot` object cost 47 to 50 ns (4.4x to 4.6x of `std::make_shared`).

## Quick start

Hot++ needs CMake 3.20 and a Clang C++ compiler with `libclang`. The JIT also needs the LLVM and Clang development packages.

```cmake
set(CMAKE_CXX_STANDARD 17)
set(HOT_ROOT "/path/to/hotpp" CACHE PATH "The Hot++ source tree")
add_subdirectory(${HOT_ROOT} hot)
add_executable(myapp main.cpp)
hot_add_host(TARGET myapp MODE HOT_RELOAD SCRIPTS wolf.hot
             INCLUDES ${CMAKE_CURRENT_SOURCE_DIR} DEPENDS Creature.h)
```

`MODE` is `AOT`, `AOT_RELOAD`, `HOT_RELOAD` or `PRECOMPILED`. See [CMake](cmake.html). To build Hot++ and run its tests, use `cmake --preset linux-clang-files`, then `cmake --build --preset linux-clang-files-dbg` and `ctest --preset linux-clang-files-dbg`.

## Documentation

- [Getting started](getting-started.html): write a class, check it and compile it.
- [Hot reload](hot-reload.html): how a reload swaps the code of a running object.
- [The `.hot` language](types.html): types, classes, inheritance, native interop.
- [C++ scripts](cpp-scripts.html): a script as a plain `.cpp` file.
- [Embedding](embedding.html): the six hosting situations and the API.
- [CMake](cmake.html): `hot_add_host` and the options.
- [Tools](tools.html): `hotc`, the language server `hot-ls`, the VS Code extension.
- [Benchmarks](benchmarks.html) and [Development notes](development.html).

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

## The generated C++

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

## Status

- Linux: tested with Clang, also under ASan and TSan.
- Windows: partial. The code paths are not verified.
- macOS: untested.
- Coroutines are not in version 1.

## License

MIT license, see LICENSE.
