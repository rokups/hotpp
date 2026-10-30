---
layout: default
title: Getting started
nav_order: 2
---

A script is a class in a `.hot` file. It derives from a native engine class.

A script can also be a plain C++ file, a `.cpp` script. It compiles unchanged in a normal build. See [C++ scripts](cpp-scripts.html).

## Write a class

`using` pulls in a native header. A method with `override` replaces a native virtual method. A bare name reaches a script member or a native member of the base. Use `this.` only when a parameter has the same name.

```spp
using "Actor.h";
using "Math.h";

class Player : Actor {
    float hp = 100.0;
    float speed = 5.0;

    override void Tick(float dt) {
        Move(Vec3(speed * dt, 0, 0));
        heal(1.0);
    }

    void heal(float amount) {
        hp = Math.Min(hp + amount, 100.0);
    }
}
```

## Check and compile

The first command translates the script to C++ and prints it. The second command builds a shared library for generation 1.

```
hotc --emit-cpp player.hot --gen 1 -I include -I tests/engine
hotc player.hot --gen 1 -I include -I tests/engine -o player.so
```

The scripts of this page use the headers of the test host in `tests/engine` (`Actor.h`). For your own project, use your own native headers. The folder `tests/engine` is a test host and is not a model for a user project. Copy from `samples/` (see `samples/README.md`).
