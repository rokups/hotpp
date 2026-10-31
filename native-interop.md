---
layout: default
title: Native interop
nav_order: 11
group: Hot script
---

A script calls engine code directly. There is no wrapper code.

## Engine functions and C++ types

Include the header with `using`. Then call its functions, methods and types. Use `.` for every scope. Use `T(args)` to build a value.

```hot
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
        float d = Distance(Position, Vec3(0, 0, 0));   // free function
        Name = FormatVec(Position);               // returns std.string
        history.push_back(static_cast<int>(history.size()));
    }
}
```

## Native objects as parameters

Take a native object as `T&`. You call its methods and read its members. The overload comes from the argument types.

```hot
using "Actor.h";

class Hitter : Actor {
    void Hit(Actor& other) {
        other.TakeDamage(5.0);
        other.TakeDamage(*this, 2.0);
        other.Move(Vec3(1, 0, 0));
        var name = other.Name;          // a copy of the member
    }
}
```

## Native API patterns

The translator reads the native signature. You write the call in the plain form.

```cpp
// Armory.h (engine)
struct Armory {
    bool Damage(int id, int& dmg);           // out-parameter
    Weapon* Best();                          // can return nullptr
    bool Find(int id, Weapon** out);         // out-pointer
    std::optional<int> Pick(int id);
    std::shared_ptr<Vec3> Origin();
    void Hit(const Weapon& w);
    void Use(Weapon* w);
    std::vector<int> ids;
};
```

```hot
using "Armory.h";

[native, handle("std::shared_ptr<$T>")] class Weapon;

class Shop : Actor {
    int total = 0;

    void Run(Armory& a, Weapon h) {
        int dmg = 0;
        if (a.Damage(7, dmg)) total += dmg;       // T& out-parameter: no marker

        Weapon* best = a.Best();                       // T* result: compare with null
        if (best != null) total += best.dmg;

        Weapon* w = null;
        if (a.Find(7, &w)) total += w.dmg;        // Weapon** takes &w

        var it = a.ids.begin();
        total += *it;                             // iterator

        var o = a.Pick(3);
        if (o.has_value()) total += *o;           // std.optional

        var p = a.Origin();
        float x = (*p).x;                              // smart pointer

        a.Hit(*h);                                     // handle to const T&
        a.Use(&*h);                                    // handle to T*
    }
}
```

`&h` on a handle is an error. Pass `h` for a handle parameter, `&*h` for a `T*` and `*h` for a `T&`. `.` on a handle always means a member of the pointee, so `h.get()` calls `get` of the pointee. Inside a method, `this` is a handle for a handle parameter. For a native `Actor&` parameter, write `*this`. A `T&` parameter of a script class takes a variable, not `this`: write `Hero me = this;` first.

## Fallback handle

`[native, default_handle("std::shared_ptr<$T>")] class *;` sets the handle for every native class that has no stub of its own.

```hot
using "Weapon.h";

[native, default_handle("std::shared_ptr<$T>")] class *;

class Smith {
    Weapon w = null;       // Weapon has no stub: it gets the fallback handle
}
```

## Types with operator->

`.` works on a value, a pointer, a reference and a `std.shared_ptr`. It does not work on an iterator, a `std.optional` or a `std.unique_ptr`. Write `(*p).x` for these types.

```hot
using "Actor.h";

class Holder : Actor {
    void Run(std.unique_ptr<Actor>& p) {
        // p.Position.x does not compile
        float x = (*p).Position.x;
    }
}
```

## Other limits

- Write `.`, not `::` or `->`.
- A native array has no `.` access.
- A native static data member, such as `Vec3.Zero.x`, needs a typed local first.
- A function that the script calls must be exported by the host. Link with `ENABLE_EXPORTS`.
