---
layout: default
title: Native interop
nav_order: 9
---

A script calls engine code directly. There is no wrapper code.

## Engine functions and C++ types

Include the header with `using`. Then call its functions, methods and types. Use `.` for every scope. Use `T(args)` to build a value.

```csharp
using <vector>;
using <string>;
using <algorithm>;
using "Actor.h";
using "Math.h";
using "Util.h";

class Tracker : Actor {
    std.vector<int> history = std.vector<int>();
    std.string label = "tracker";

    override void Tick(float dt) {
        var v = std.vector<float>();                        // template from C++
        v.push_back(4.0);
        v.push_back(2.5);
        std.sort(v.begin(), v.end(), std.greater<float>());
        float low = Math.Min(v[0], v[1]);                   // static function
        float d = Distance(this.Position, Vec3(0, 0, 0));   // free function
        this.Name = FormatVec(this.Position);               // returns std.string
        this.history.push_back(static_cast<int>(this.history.size()));
    }
}
```

## Native objects as parameters

Take a native object as `T&`. You call its methods and read its members. The overload comes from the argument types.

```csharp
using "Actor.h";

class Hitter : Actor {
    void Hit(Actor& other) {
        other.TakeDamage(5.0);
        other.TakeDamage(this, 2.0);
        other.Move(Vec3(1, 0, 0));
        var name = other.Name;          // a copy of the member
    }
}
```

## ref and out

Mark the parameter and the argument. The native side gets a reference.

```csharp
using "Actor.h";

class Mover : Actor {
    void Split(float total, out float half, ref int calls) {
        half = total / 2.0;
        calls++;
    }

    override void Tick(float dt) {
        float h = 0.0;
        int n = 0;
        this.Split(dt, out h, ref n);
    }
}
```

## Types with operator->

`.` works on a value, a pointer and a reference. It does not work on a type that has `operator->`: a smart pointer, an iterator or `std::optional`. Call `.get()` first.

```csharp
using <memory>;
using "Actor.h";

class Holder : Actor {
    std.unique_ptr<Actor> p;

    void Run() {
        // this.p.Position.x does not compile
        float x = this.p.get().Position.x;
    }
}
```

## Other limits

- Write `.`, not `::` or `->`.
- A native array has no `.` access.
- A native static data member, such as `Vec3.Zero.x`, needs a typed local first.
- A function that the script calls must be exported by the host. Link with `ENABLE_EXPORTS`.
