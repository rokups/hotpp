---
layout: default
title: Borrowing
nav_order: 6
group: Hot script
---

A borrowed value is a reference or a pointer to an object that something else owns. A script can use it during one call. A script cannot store it.

## Parts of this guide

This guide has 2 pages. This page covers what borrowing is, where it is allowed and rejected, and handles.

- [Borrowed locals, calls and fixes](borrowing-locals.html): borrowed locals, borrowed values in calls, the escape hatch and quick fixes.

## What borrowing is

A value (`Vec3`) is a copy. A handle (a script class, or a native class with a stub) is a smart pointer. A borrowed value is a `ref` or `in` parameter, a `T*` or a `ref` local. The state block of a script object moves when the script reloads. A stored pointer into the block would dangle. A handle is safe: its pointee does not move.

```hot
using "Actor.h";

class Hero : Actor {
    Vec3 home;                      // value: a copy in the state block
    $Hero buddy = null;              // handle: safe to store
    void Move(ref Vec3 to) { }         // borrowed: valid during the call
}
```

## Where borrowing is allowed

| Position | Example | Allowed |
|---|---|---|
| Parameter | `void F(ref Vec3 v)`, `void F(Vec3* v)` | yes |
| Local | `ref r = x;`, `Vec3* p = a.Best();`, `var p = a.Best();` | yes |
| `catch`, lambda parameter, `foreach` | `catch (std.exception e)`, `(ref Vec3 v) => ...`, `foreach (ref x in xs)` | yes |
| A value lvalue in a call to a native `T*` parameter | `Nudge(x)`, `Peek(this.pos)` | yes: the call writes `hot::as_ptr(x)` |
| A `T*` parameter passed on in a call, `new` or `base(...)` | `f(p)`, `new $Hero(p)` | yes |
| Field of a native class with a weak source | `Weapon* last;` | yes: stored as a checked pointer (see "Pointer fields") |
| Field of another type, return type, collection element, lambda capture | `ref Vec3 last;`, `() => p.x` | no |

```hot
using "Actor.h";
class Hero {
    Hero(int* n) { }
    void Show(ref Vec3 p) { }
    void Run(ref Vec3 pos, int total) {                   // parameter
        try { Show(pos); }
        catch (std.exception e) { Show(pos); }   // catch
        var f = (ref Vec3 v) => { Show(v); };        // lambda parameter
        std.vector<int> xs = std.vector<int>();
        foreach (ref x in xs) x = x + 1;              // foreach
        ref r = total;                                // local reference
        var h = new $Hero(null);                        // null for a T* parameter
    }
}
```

## Where borrowing is rejected

A field, a return value, a collection element and a lambda capture are errors. The prefix `&` does not exist in `.hot`. A method that gives access to a value struct returns `ref T` or `T?`, not `T*`: see [Type design](type-design.html#lifetime-of-a-ref). A field of the move-only type `std.unique_ptr<T>` is also an error, because a reload copies each field. A pointer field of a native class with a weak source is the exception (see "Pointer fields").

```hot
// error
using "Actor.h";
class Bad {
    Vec3 last;
    Vec3* ptr;                          // pointer to a value type
    std.vector<Vec3*> all;              // collection element
    std.unique_ptr<Vec3> own;           // move-only field
    void Run(ref Vec3 v) {
        var f = () => { v.x = 1; };     // capture
    }
}
```

```
a field cannot hold a pointer to the value type 'Vec3': nothing owns it, so the pointer can dangle. Use a value, or a pointer to the object that owns it
a collection element cannot hold a borrowed pointer or reference to 'Vec3'. Declare a stub ...
field of move-only type 'std::unique_ptr<Vec3>' is not supported: a reload copies each field. Use a handle
the lambda captures 'v', a borrowed pointer or reference. A closure cannot keep it. Copy the value or use a handle
```

## Handles and borrowing

`.` on a handle means a member of the pointee. Pass `h` for a handle parameter, or for a native `T*` or `T&` parameter: the translator converts the handle (see "Native objects as parameters" in `native-interop.md`). The conversion needs the exact class of the parameter. For a handle of a derived class, write the cast `(T*)h` for a `T*` and `*h` for a `T&`. The cast is a checked unwrap: a null handle throws `NullReference`. A `ref` parameter of a script class is a reference to the handle, so the callee can rebind it. `this` is not a variable: copy it to a local first. An intrusive stub is an exception: its `T*` is the handle, so a field can have it.

```hot
using "Actor.h"; using "Weapon.h"; using "Armory.h"; using "Object.h";
[native, handle("std::shared_ptr<$T>")] class Weapon;
[native, handle("$T*"), intrusive("AddRef", "Release")] class Object;

class Hero : Actor {
    $Hero buddy = null;
    Object* rc = null;                           // intrusive: the pointer is the handle
    void Pair(ref $Hero dst) { dst = buddy; }   // dst is the handle itself
    void Join(ref $Hero other) { }
    void Run(ref Armory a, Weapon h) {
        a.Use(h); a.Hit(h);              // native T* parameter, native T& parameter: the plain handle
        a.Use((Weapon*)h); a.Hit(*h);    // the same, written out
        $Hero me = this; Join(me);        // this is not a variable: pass a copy
    }
}
```

An override can take a native `Weapon*` or `Weapon&` as the plain handle `Weapon`. The body then owns a handle, not a borrowed pointer. See "Parameter forms of an override" in `inheritance.md`.

A `Weapon*` field is not a finding when `Weapon` has a weak source. The field is a checked pointer (see "Pointer fields"). A `Weapon*` field of a class with no weak source is a finding. `Hero*` of a script class is always an error. The `this` error shows after the other errors are fixed.

A weak handle `[weak] T x` does not keep its object alive, so it breaks a cycle of handles. Each read locks it for one statement. So `ref r = x.hp` is an error: the reference dangles. See "Weak handles" in `types-handles.md`.

```hot
// error
using "Actor.h"; using "Mesh.h"; using "Armory.h";
[native] class Mesh;
[native, handle("std::shared_ptr<$T>")] class Weapon;

class Hero : Actor {
    Mesh* last;
    Hero* buddy;
    void Join(ref $Hero other) { }
    void Run(ref Armory a, Weapon h) {
        Join(this);
    }
}
```

```
a field cannot hold a borrowed pointer or reference to 'Mesh'. Declare a stub with 'handle' or 'intrusive' for 'Mesh', or use a value
a pointer to the script class 'Hero' is not allowed: use '$Hero' (a handle)
'this' cannot bind to the reference parameter 'other' of the script class 'Hero'. Copy it to a local first: '$Hero me = this;'
```

## Pointer fields

A field or a module variable of type `T*` is allowed when `T` is a native class with a weak source. The translator stores it as `hot::CheckedPtr<T>`. The pointer is weak: it does not keep the object alive. It reads null after the object dies. It keeps its target across a reload.

A class with a raw pointer field needs a destructor that uses the field. A field of type `std.vector<T*>` or `std.map<K, T*>` is also a raw pointer field. The destructor must name the field: it can delete the object, set the field to `null`, pass it on, or read it. This way the user decides who frees the object. A class with no destructor, or a destructor that does not name the field, is an error at the field. A handle, a `std.shared_ptr`, a `std.unique_ptr`, an intrusive handle and a `const char*` are not raw pointer fields. A script class in an import has this check only in the module that owns it. A `[weak]` tag does not turn the check off.

```
class Tool : Widget {
    Mesh* mesh;
    ~Tool() { mesh = null; }
}
```

A weak source is one of these:

- The class derives from `std::enable_shared_from_this`.
- A stub gives the class a non-intrusive `handle` or `weak_handle`.
- The host specializes `hot::weak_ref_traits<T>` (see "Custom handles" in `types-handles.md`).
- A stub has the attribute `weak_source("Base<$T>")` (see "A class that no handle owns").

```hot
using "Actor.h"; using "Equip.h";

class Hero : Actor {
    Charm* target = null;                  // a checked pointer
    override void Tick(float dt) {
        target?.Bump(1);   // a dead target reads as null: the call does nothing
    }
    ~Hero() { target = null; }
}
```

The use is the same as for a pointer. `p == null`, `p = null`, `p.M()`, and `Use(p)` for a native `T*` parameter work. A call on a null or dead pointer throws `NullReference`. A native call that returns a raw `T*` can assign to the field when the class has `std::enable_shared_from_this`. An object that no `std::shared_ptr` owns throws `NullReference` at the assignment. A class with a non-intrusive stub and no `enable_shared_from_this` accepts a handle only: `p = h;`.

The object can die while a native call uses it. A native `T&` from `*p` is borrowed, like a raw pointer. Do not keep it.

A field with an intrusive stub stays the handle: `Object*` is the handle (see above). These forms are errors:

| Form | Error |
|---|---|
| A reference field | `"&" is not a type suffix; write ref T x`. A field cannot be a reference. Use `T*` or a value. |
| `int* p;`, `byte* b;`, a pointer to an enum | `a field cannot hold a pointer to the value type 'int': nothing owns it, so the pointer can dangle. Use a value, or a pointer to the object that owns it` |
| `[weak] Weapon* w;` | `a pointer field is already a weak reference: remove '[weak]'` |
| `Mesh* m;` where `Mesh` has no weak source | `a field cannot hold a borrowed pointer or reference to 'Mesh'. Declare a stub with 'handle' or 'intrusive' for 'Mesh', or use a value` |
| `Weapon**`, `void*`, a return type or a collection element `Weapon*` | the borrowed message above |

A pointer to a script class is still an error. Use the handle `Hero`.

### A class that no handle owns

Some native classes have no owner that a script can see. They do not derive from `RefCounted`, no `std::shared_ptr` owns them, and a stub with a handle does not fit. Examples are `ImFont` and `ConnectionCloser`. A raw pointer to such an object can dangle. The attribute `weak_source` marks the class as a weak source. The class has no `handle`, `intrusive` or `value`. The text is the base of `hot::weak_ref_traits<T>` and has `$T`. The module header writes the traits.

```
[native, weak_source("hot::registered_weak_ref<$T>")] class ImFont;
```

The runtime has `hot::registered_weak_ref<T>` and `hot::weak_registry<T>` (`hot/runtime/weak_registry.h`). The host calls `hot::weak_registry<T>::add(p)` when an object starts to live. The host calls `remove(p)` before it dies. A field `T*` is then a checked pointer. It reads null after `remove(p)`. A new object at the same address does not revive an old pointer. A raw pointer that the host did not add throws `NullReference` at the assignment.

The rules:

- The host must call `add` and `remove` for each object. A missing `remove` leaves a dangling pointer: the registry cannot see the death.
- The registry is one for each class in the whole process. `add` and `remove` can run on any thread.
- A host that reads the field with `hot::field_ptr<hot::CheckedPtr<T>>` writes the same specialization: `template <> struct hot::weak_ref_traits<T> : hot::registered_weak_ref<T> {};`
- A different base is possible for a class with its own token: the base needs `strong_type`, `weak_type`, `to_weak`, `lock` and, for an assignment from `T*`, `from_raw` (see `checked_ptr.h`).
- `weak_source` is an error on a stub with `handle`, `intrusive` or `value`, on `class *`, and without `$T`.

A pointer field of a class with a weak source still needs the destructor rule above.
