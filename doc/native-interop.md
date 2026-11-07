---
layout: default
title: Native interop
nav_order: 16
group: Hot script
---

A script calls engine code directly. There is no wrapper code.

## Parts of this guide

This guide has 4 pages. This page covers engine functions, native objects as parameters, native API patterns and the fallback handle.

- [Scope of a stub and C++ insertions](native-interop-scope.html): the scope of a stub and the C++ insertions.
- [operator->, stub checks and index bounds](native-interop-checks.html): types with `operator->`, the checks on a stub and the bounds check on an index.
- [Module attributes and other limits](native-interop-attributes.html): module attributes, the array type and other limits.

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

Take a native object as `ref T x` or `in T x`. You call its methods and read its members. The overload comes from the argument types.

```hot
using "Actor.h";

class Hitter : Actor {
    void Hit(ref Actor other) {
        other.TakeDamage(5.0);
        other.TakeDamage(*this, 2.0);
        other.Move(Vec3(1, 0, 0));
        var name = other.Name;          // a copy of the member
    }
}
```

A script handle `Weapon w` goes to a native `Use(Weapon*)` or `Use(const Weapon&)` in the plain form: `Use(w)`. The translator writes `hot::as_ptr(w)` for `T*` and `const T*`. It writes `hot::as_ref(w)` for `T&` and `const T&`. A null handle gives `nullptr` for a pointer. A null handle throws `NullReference` for a reference. This check is always on, also with `HOT_RUNTIME_CHECKS=0`. A parameter of the handle type (`std::shared_ptr<Weapon>`) takes `w` as it is.

The conversion needs a clear parameter form. All overloads of the function with this number of arguments must take the argument in the same form. When the overloads disagree, for example `Mixed(Weapon*)` and `Mixed(const Weapon&)`, nothing converts. Write `Mixed((Weapon*)w)` or `Mixed(*w)`. The conversion applies to an argument that is a variable, a field or a member chain. Other expressions keep their form. A `.cpp` script does not convert.

A value lvalue `x` of type `T` goes to a native `T*` or `const T*` parameter in the plain form `Nudge(x)`. The translator writes `hot::as_ptr(x)`. The argument is a variable, a field, a member chain or an index. A `ref` local or parameter works too. A temporary is an error at the C++ compile. There is no prefix `&` in `.hot`; `Nudge(&x)` is a syntax error. A `.cpp` script keeps the C++ `&`.

A scalar (`int`, `float`, `bool` and so on) goes only to a `T*` that is not `const`. This is the out-parameter shape: `Bump(n, 2)` with `void Bump(int* v, int by)`. A `const float*` parameter takes a data pointer, and native code can read past the one element. The translator refuses `F(x)` for a `const` scalar pointer with a clear error. Use an array type such as `float[]` or a C++ helper that takes the value. A value struct (`Vec3`) goes to `T*` and to `const T*`.

A native `T**` out-pointer parameter takes a local of type `T*`: `char* end = null; float v = ParseFloat(s, end);` for `float ParseFloat(const char* s, char** end)`. This is the form of `strtof(s, &end)`. The translator writes `hot::as_out_ptr(end)`. The argument must be a local variable of type `T*`. A field, a temporary or another type is an error. The local is a borrowed pointer: do not store it or return it. The form is for `T**` only. A `T***`, a `const T**` and a `T*&` parameter have no `.hot` form: wrap the call in a C++ helper.

A pointer from a temporary handle is valid to the end of the full expression. Do not store it. A handle in a local or a field keeps the object alive.

## Native API patterns

The translator reads the native signature. You write the call in the plain form.

```cpp
// Armory.h (engine)
struct Armory {
    bool Damage(int id, int& dmg);           // out-parameter
    Weapon* Best();                          // can return nullptr
    void Nudge(Vec3* v);                     // pointer to a value
    std::optional<int> Pick(int id);
    std::shared_ptr<Vec3> Origin();
    void Hit(const Weapon& w);
    void Use(Weapon* w);
    std::vector<int> ids;
};
```

```hot
using "Actor.h";
using "Armory.h";

[native, handle("std::shared_ptr<$T>")] class Weapon;

class Shop : Actor {
    int total = 0;

    void Run(ref Armory a, Weapon h) {
        int dmg = 0;
        if (a.Damage(7, dmg)) total += dmg;       // native T& out-parameter: no marker

        Weapon* best = a.Best();                       // T* result: compare with null
        if (best != null) total += best.dmg;

        Vec3 pos;
        a.Nudge(pos);                             // a value lvalue to Vec3*: hot::as_ptr(pos)

        var it = a.ids.begin();
        total += *it;                             // iterator

        var o = a.Pick(3);
        total += o ?? 0;                          // std.optional: the value, or 0

        var p = a.Origin();
        float x = (*p).x;                              // smart pointer

        a.Hit(*h);                                     // handle to const T&
        a.Use((Weapon*)h);                             // handle to T*, checked for null
    }
}
```

Pass `h` for a handle parameter, or for a native `T*` or `T&` parameter. The conversion needs the exact class of the parameter. For a handle of a derived class, write `(Base*)h` for a `T*` and `*h` for a `T&`. The cast is a checked unwrap: a null handle throws `NullReference`. `.` on a handle always means a member of the pointee, so `h.get()` calls `get` of the pointee. Inside a method, `this` is a handle for a handle parameter. For a native `Actor&` parameter, write `*this`. A `T&` parameter of a script class takes a variable, not `this`: write `$Hero me = this;` first.

## Fallback handle

`[native, default_handle("std::shared_ptr<$T>")] class *;` sets the handle for every native class that has no stub of its own.

```hot
using "Weapon.h";

[native, default_handle("std::shared_ptr<$T>")] class *;

class Smith {
    Weapon w = null;       // Weapon has no stub: it gets the fallback handle
}
```
