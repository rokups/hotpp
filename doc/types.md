---
layout: default
title: Types
nav_order: 3
---

S++ has fixed-width builtin types. Other types come from C++.

## Builtin types

| Type | Alias | Note |
|---|---|---|
| `bool` | | |
| `sbyte` | `int8` | |
| `byte` | `uint8` | |
| `short` | `int16` | |
| `ushort` | `uint16` | |
| `int` | `int32` | `5` |
| `uint` | `uint32` | |
| `long` | `int64` | `5L` |
| `ulong` | `uint64` | |
| `nint`, `nuint` | | pointer size |
| `float` | | `5.0` |
| `double` | | `5.0d` |
| `char`, `wchar` | | 8 bit and 16 bit |
| `void` | | no value |

`null` is the empty handle.

## Variables

A variable can have no initializer. It then gets the default value (zero, `false`, `null`). `var` takes the type from the initializer. `const` makes the variable read only.

```csharp
class Demo {
    int hp = 100;
    ulong id = 7;
    float speed = 5.0;         // 5.0 is float, 5.0d is double
    double ratio = 0.5d;
    long big = 5L;
    int count;                 // default value: 0

    void Run() {
        var x = 3;             // int
        const int limit = 10;  // cannot change
    }
}
```

## C++ types

Use `.` for a scope. The translator writes `::` and `->` for you.

```csharp
using <string>;
using <vector>;

class Names {
    std.string name = "demo";
    std.vector<int> list = std.vector<int>();
}
```

## Handles

A script class is a handle. `new` makes the object and `null` clears the handle. A native class needs a stub to be a handle.

```csharp
using "Actor.h";
using "Weapon.h";

[native, handle("std::shared_ptr<$T>")] class Weapon;

class Knight : Actor {
    Weapon weapon = null;

    override void Tick(float dt) {
        if (this.weapon == null) this.weapon = new Weapon(7, "sword");
    }
}
```

## References and pointers

A parameter `T&` is a reference. The call has no marker: write `f(x)`. A reference is borrowed: use it in the method, but do not store it in a field.

`&x` is the address of `x` and has the type `T*`: write `f(&x)`. `*e` reads through an iterator, an `std.optional`, a smart pointer or a handle. `&h` on a handle is an error: [Native interop](native-interop.html) shows the right forms.

`var& r = x;` and `const var& r = x;` make a reference local. `var r = x;` copies.

```csharp
using <vector>;
using "Actor.h";

class Bullet {
    int dmg = 1;
}

class Mover : Actor {
    Bullet shot = null;
    int n = 0;

    void Split(float total, float& half, int& calls) {
        half = total / 2.0;
        calls++;
    }

    void Make(Bullet& b) { b = new Bullet(); }            // b is the handle itself

    override void Tick(float dt) {
        float h = 0.0;
        int calls = 0;
        this.Split(dt, h, calls);                         // no marker
        this.Make(this.shot);

        var& r = this.n;                                  // a reference, not a copy
        r = 2;
        var copy = this.n;
        std.vector<int> xs = std.vector<int>();
        foreach (var& x in xs) x = x + copy;
        foreach (const var& x in xs) calls += x;
    }
}
```

A `T&` of a script class is a reference to the handle, so the callee can rebind it. A single `T*` of a script class is an error: write `T`. A native handle type keeps the raw `T&` and `T*`.

[Borrowing](borrowing.html) lists every place where a reference or a pointer is allowed.

Not supported: generics in script code, `?.`, `::`, `->` and brace initializers. Write `T(args)` instead of `T{args}`.
