---
layout: default
title: Borrowed locals, calls and fixes
nav_order: 7
group: Hot script
---

## Borrowed locals

A local is borrowed when it is `ref r = x;`, a typed `T*` or a `ref T r` local, or a `var` that takes a native pointer from a call. The prefix `&` does not exist in `.hot`: write `ref r = x;` for a local reference. A native pointer to a handle type is not borrowed. A pointer field of a native class with a weak source is a checked pointer, not a borrowed value (see "Pointer fields" in `borrowing.md`). A lambda that uses a borrowed local gives the capture error.

A `ref` local has more rules about its lifetime. A `ref` cannot bind to a temporary. A `ref` to a container element is a checked path. A shape change of the container, such as `push_back`, is safe. A `ref` from a `ref` call must not live over a shape change of its root. A `ref` cannot cross an `await`. A `ref` return must return a place that outlives the call. These rules are always errors. See "Lifetime of a ref" in [Type design](type-design.html#lifetime-of-a-ref).

```hot
using "Actor.h"; using "Armory.h";
class Scan : Actor {
    void Run(ref Armory a, ref Vec3 pos) {
        ref r = pos.x;                  // borrowed: reference
        var best = a.Best();             // borrowed: native Weapon*
        var copy = r;                    // a value ...
        var ok = () => { var v = copy; };  // ... so a lambda can capture it
    }
}
```

Known gaps of the checker: a call can still store its borrowed argument in a collection that a field holds, for example `items.push_back(p)` where `items` is a field of a pointer collection type and `p` is a `T*` parameter. The checker does not follow a native call into the callee. a lambda that captures an iterator (`var it = xs.begin();`) or an `std.optional<T&>` is not found.

## Borrowed values in calls

A borrowed value is valid for the whole call. Every call form takes it as an argument. The callee must not keep it.

| Call form | Example |
|---|---|
| Native method, static function, free function | `a.Use(x)`, `Util.Sum(r)`, `Distance(v, *p)` |
| Script method, also through a handle | `Show(v)`, `buddy.Show(x)` |
| Constructor, `new`, `base(...)` | `new $Hero(x)`, `Hero(ref Vec3 r) : base(r)` |
| Lambda call | `f(v)`, `g(x)` |
| Nested call | `f(g(x))` |
| Choice, cast, dereference | `f(c ? a : b)`, `f((Vec3*)p)`, `f(*(c ? p : q))` |
| Parts | `f(a.field)`, `f(arr[i])`, `f(*p)` |
| Borrowed names | `f(p)`, `f(r)`, a `foreach` variable, a `catch` variable |

A value lvalue goes to a native `T*` or `const T*` parameter in the plain form: `Nudge(a)` for `void Nudge(Vec3* v)`. The translator writes `hot::as_ptr(a)`. A scalar goes only to a `T*` that is not `const`. A temporary does not bind: the C++ compiler reports an error. A local `char* end = null` goes to a native `char**` parameter as `F(s, end)`; the local is a borrowed pointer. A script function does not take a value for a `T*` parameter. A `T*` parameter of a script function takes `null` or a native pointer. For a script caller, write `ref T x`.

A call can return a pointer or a reference to its argument. The translator does not follow this alias. A `var` that takes a native pointer from a call is borrowed. A `var` that takes a reference from a call is a copy. Do not store the result of such a call. A `ref` local can take a native `T&` result: see "Lifetime of a ref" in [Type design](type-design.html#lifetime-of-a-ref).

```hot
using "Actor.h"; using "Util.h";
class Probe {
    void Take(Vec3* p) { }
    void See(ref Vec3 v) { }
    float Run(ref Vec3 v, Vec3* p, bool c) {
        if (p == null) return 0;
        Vec3 a; Vec3 b;
        Take(c ? p : null);             // every call form takes it ...
        See(c ? a : b);
        Nudge(a);
        var f = (ref Vec3 w) => { See(w); };
        f(v);
        return Distance(v, *p);
    }                                   // ... but nothing keeps it
}
```

## Escape hatch

`hotc --no-strict-borrowed` makes the field, return, collection element and pointer lambda capture findings warnings. The backends have `strict_borrowed = false`. These stay errors: all rules of "Lifetime of a ref" (including a `ref` lambda capture), a `T*` of a script class, `this` to a `T&` and a move-only field.

## Quick fixes

| Error starts with | Write instead |
|---|---|
| `"&" is not a type suffix` | `ref T x` for a parameter or a local. A field cannot be a reference: use `T*` or a value |
| `a field cannot hold a pointer to the value type` | A value `T`, or a pointer to the object that owns it |
| `a pointer field is already a weak reference` | Remove `[weak]` |
| `a field`, `a return value`, `a collection element` | A value `T`, a handle, or a copy such as `std.string`. For a field of a native class: add a weak source (`enable_shared_from_this` or a stub with `handle`) and keep `T*` |
| `the prefix '&' is not supported` | Pass the value: `F(x)` for a native `T*` parameter. Write `ref r = x;` for a local reference. Write `(T*)h` for the pointer of a handle |
| `binds to a temporary value` | `var r = ...;` to copy the value |
| `can move or remove the elements` | `var r = ...;`, or end the ref before the call |
| `is live across 'await'` | Copy the value before the `await` |
| `the lambda captures` | Copy first: `var c = r;`, then use `c` |
| `field of move-only type` | A handle: a stub with `handle` |
| `a pointer to the script class` | `$Hero` |
| `'this' cannot bind` | `$Hero me = this;` |
