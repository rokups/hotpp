---
layout: default
title: Borrowing
nav_order: 4
---

A borrowed value is a reference or a pointer to an object that something else owns. A script can use it during one call. A script cannot store it.

## What borrowing is

A value (`Vec3`) is a copy. A handle (a script class, or a native class with a stub) is a smart pointer. A borrowed value is `T&`, `T*`, `var&` or `&x`. The state block of a script object moves when the script reloads. A stored pointer into the block would dangle. A handle is safe: its pointee does not move.

```csharp
using "Actor.h";

class Hero : Actor {
    Vec3 home;                      // value: a copy in the state block
    Hero buddy = null;              // handle: safe to store
    void Move(Vec3& to) { }         // borrowed: valid during the call
}
```

## Where borrowing is allowed

| Position | Example | Allowed |
|---|---|---|
| Parameter | `void F(Vec3& v)`, `void F(Vec3* v)` | yes |
| Local | `var& r = x;`, `Vec3* p = &x;`, `var p = &x;` | yes |
| `catch`, lambda parameter, `foreach` | `catch (std.exception& e)`, `(Vec3& v) => ...`, `foreach (var& x in xs)` | yes |
| `&x` in a call, `new` or `base(...)` | `f(&x)`, `new Hero(&x)` | yes |
| Field, return type, collection element, lambda capture | `Vec3& last;`, `() => p.x` | no |
| `&x` in an assignment or `return` | `this.p = &x;` | no |

```csharp
using <vector>; using <exception>; using "Actor.h";
class Hero {
    Hero(int* n) { }
    void Show(Vec3& p) { }
    void Run(Vec3& pos, int total) {                   // parameter
        try { this.Show(pos); }
        catch (std.exception& e) { this.Show(pos); }   // catch
        var f = (Vec3& v) => { this.Show(v); };        // lambda parameter
        std.vector<int> xs = std.vector<int>();
        foreach (var& x in xs) x = x + 1;              // foreach
        var& r = total;                                // local reference
        var p = &pos;                                  // local pointer
        var h = new Hero(&total);                      // &x as new argument
    }
}
```

## Where borrowing is rejected

A field, a return value, a collection element, a lambda capture and a stored `&x` are errors. A field of the move-only type `std.unique_ptr<T>` is also an error, because a reload copies each field.

```csharp
// error
using <vector>; using <memory>; using "Actor.h";
class Bad {
    Vec3& last;                         // field
    std.vector<Vec3*> all;              // collection element
    std.unique_ptr<Vec3> own;           // move-only field
    Vec3* Addr() { return &this.last; } // return value, return &x
    void Run(Vec3& v) {
        this.last = &v;                 // assignment of &x
        var f = () => { v.x = 1; };     // capture
    }
}
```

```
a field cannot hold a borrowed pointer or reference to 'Vec3'. Declare a stub with 'handle' or 'intrusive' for 'Vec3', or use a value
a collection element cannot hold a borrowed pointer or reference to 'Vec3'. Declare a stub ...
field of move-only type 'std::unique_ptr<Vec3>' is not supported: a reload copies each field. Use a handle
a return value cannot hold a borrowed pointer or reference to 'Vec3'. Declare a stub ...
a '&' value cannot be stored in an assignment, a 'return' or a field initializer. Use '&' only for a call argument or a local
the lambda captures 'v', a borrowed pointer or reference. A closure cannot keep it. Copy the value or use a handle
```

## Handles and borrowing

`.` on a handle means a member of the pointee. `&h` on a handle is an error: pass `h` for a handle parameter, `&*h` for a `T*` and `*h` for a `T&`. A `T&` of a script class is a reference to the handle, so the callee can rebind it. `this` is not a variable: copy it to a local first. An intrusive stub is an exception: its `T*` is the handle, so a field can have it.

```csharp
using "Actor.h"; using "Weapon.h"; using "Armory.h"; using "Object.h";
[native, handle("std::shared_ptr<$T>")] class Weapon;
[native, handle("$T*"), intrusive("AddRef", "Release")] class Object;

class Hero : Actor {
    Hero buddy = null;
    Object* rc = null;                           // intrusive: the pointer is the handle
    void Pair(Hero& out) { out = this.buddy; }   // out is the handle itself
    void Join(Hero& other) { }
    void Run(Armory& a, Weapon h) {
        a.Use(&*h); a.Hit(*h);           // T* parameter, T& parameter
        Hero me = this; this.Join(me);   // this is not a variable: pass a copy
    }
}
```

`Weapon*` of a native handle is borrowed, so it is a finding in a field. `Hero*` of a script class is always an error. The `this` error shows after the other errors are fixed.

```csharp
// error
using "Actor.h"; using "Weapon.h"; using "Armory.h";
[native, handle("std::shared_ptr<$T>")] class Weapon;

class Hero : Actor {
    Weapon* last;
    Hero* buddy;
    void Join(Hero& other) { }
    void Run(Armory& a, Weapon h) {
        a.Use(&h);
        this.Join(this);
    }
}
```

```
a field cannot hold a borrowed pointer or reference to 'Weapon'. Declare a stub ...
a pointer to the script class 'Hero' is not allowed: use 'Hero' (a handle)
'&' on the handle 'h' is not allowed. Pass 'h' for a handle parameter, '&*h' for a 'T*' parameter, or '*h' for a 'T&' parameter
'this' cannot bind to the reference parameter 'other' of the script class 'Hero'. Copy it to a local first: 'Hero me = this;'
```

## Borrowed locals

A local is borrowed when it is `var& r = x;`, `var p = &x;`, a typed `T*` or `T&`, or a `var` that takes a native pointer from a call. A native pointer to a handle type is not borrowed. A lambda that uses a borrowed local gives the capture error.

```csharp
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

Known gaps of the checker: a lambda that captures an iterator (`var it = xs.begin();`) or an `std.optional<T&>` is not found. An assignment `q = p;` into a `var q` that is not borrowed (for example `var q = (int*)null;`) is not tracked. A cast that wraps the `&` is rejected: `var c = (Actor*)&a;` gives the `a '&' value cannot be stored` error.

## Escape hatch

`sppc --no-strict-borrowed` makes the field, return, collection element, `&x` and lambda capture findings warnings. The backends have `strict_borrowed = false`. These stay errors: a `T*` of a script class, `&h` on a handle, `this` to a `T&` and a move-only field.

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
