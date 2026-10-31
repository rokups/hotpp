---
layout: default
title: Borrowed locals, calls and fixes
nav_order: 7
group: Hot script
---

## Borrowed locals

A local is borrowed when it is `var& r = x;`, `var p = &x;`, a typed `T*` or `T&`, or a `var` that takes a native pointer from a call. A native pointer to a handle type is not borrowed. A lambda that uses a borrowed local gives the capture error.

```hot
using "Actor.h"; using "Armory.h";
class Scan : Actor {
    void Run(Armory& a, Vec3& pos) {
        var p = &pos;                    // borrowed: address
        var& r = pos.x;                  // borrowed: reference
        var best = a.Best();             // borrowed: native Weapon*
        var copy = r;                    // a value ...
        var ok = () => { var v = copy; };  // ... so a lambda can capture it
    }
}
```

Known gaps of the checker: a call can still store its borrowed argument in a collection that a field holds, for example `items.push_back(&a)` where `items` is a field of a pointer collection type. The checker does not follow a native call into the callee. a lambda that captures an iterator (`var it = xs.begin();`) or an `std.optional<T&>` is not found.

## Borrowed values in calls

A borrowed value is valid for the whole call. Every call form takes it as an argument. The callee must not keep it.

| Call form | Example |
|---|---|
| Native method, static function, free function | `a.Use(&x)`, `Util.Sum(r)`, `Distance(v, *p)` |
| Script method, also through a handle | `Show(v)`, `buddy.Show(&x)` |
| Constructor, `new`, `base(...)` | `new Hero(&x)`, `Hero(Vec3& r) : base(r)` |
| Lambda call | `f(v)`, `g(&x)` |
| Nested call | `f(g(&x))` |
| Choice, cast, dereference | `f(c ? &a : &b)`, `f((Vec3*)&a)`, `f(*(c ? &a : &b))` |
| Parts | `f(&a.field)`, `f(&arr[i])`, `f(&*p)` |
| Borrowed names | `f(p)`, `f(r)`, a `foreach` variable, a `catch` variable |

`var p = c ? &a : &b;` and `var q = (Vec3*)&a;` are borrowed locals, like `var p = &a;`.

A call can return a pointer or a reference to its argument. The translator does not follow this alias. A `var` that takes a native pointer from a call is borrowed. A `var` that takes a reference from a call is a copy. Do not store the result of such a call.

```hot
using "Actor.h"; using "Util.h";
class Probe {
    void Take(Vec3* p) { }
    void See(Vec3& v) { }
    float Run(Vec3& v, Vec3* p, bool c) {
        Vec3 a; Vec3 b;
        Take(c ? &a : &b);              // every call form takes it ...
        See(*(c ? &a : &b));
        var f = (Vec3& w) => { See(w); };
        f(v);
        return Distance(v, *p);
    }                                   // ... but nothing keeps it
}
```

## Escape hatch

`hotc --no-strict-borrowed` makes the field, return, collection element, `&x` and lambda capture findings warnings. The backends have `strict_borrowed = false`. These stay errors: a `T*` of a script class, `&h` on a handle, `this` to a `T&` and a move-only field.

## Quick fixes

| Error starts with | Write instead |
|---|---|
| `a field`, `a return value`, `a collection element` | A value `T`, a handle, or a copy such as `std.string` |
| `a '&' value cannot be stored` | Pass `&x` to the call, or store a copy of `x` |
| `the lambda captures` | Copy first: `var c = r;`, then use `c` |
| `field of move-only type` | A handle: a stub with `handle` |
| `'&' on the handle` | `h`, `&*h` or `*h` |
| `a pointer to the script class` | `Hero` |
| `'this' cannot bind` | `Hero me = this;` |
