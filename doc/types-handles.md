---
layout: default
title: Handles, references and pointers
nav_order: 5
group: Hot script
---

## Handles

A script class is a handle. `new` makes the object and `null` clears the handle. A native class needs a stub to be a handle.

With a `std::shared_ptr` handle, `new` puts the object and the control block of the handle in one block of memory:

- The handle owns the object before the constructor body runs. So the constructor can give `this` as a handle.
- When the constructor throws, the handle releases the object. A handle that the constructor gave out keeps the object alive.
- The last strong handle runs the destructor and frees the fields. A weak handle keeps only the memory of the object, until the last weak handle goes.

```hot
using "Actor.h";
using "Weapon.h";

[native, handle("std::shared_ptr<$T>")] class Weapon;

class Knight : Actor {
    Weapon weapon = null;

    override void Tick(float dt) {
        if (weapon == null) weapon = new Weapon(7, "sword");
    }
}
```

### Weak handles

A handle keeps its object alive. Two objects that hold handles to each other make a cycle. Then neither object is released. A weak handle does not keep its object alive, so it breaks the cycle. Usually the child holds a weak handle to its parent.

Put `[weak]` before the declaration. On a field, write the attribute on its own line. On a parameter or a local, write it inline.

```hot
class Parent {
    Child kid = null;
    int hp = 10;

    void Adopt(Child c) { kid = c; c.parent = this; }
}

class Child {
    [weak]
    Parent parent = null;

    int ParentHp() { return parent != null ? parent.hp : 0; }
    void See([weak] Parent p) { [weak] Parent seen = p; }
}
```

The two classes use each other, and the order in the file does not matter. See [Classes that use each other](classes-use.html#classes-that-use-each-other).

These are the rules:

- The type is a handle with a `std::shared_ptr` handle template. A script class without a native base is one. A native class needs a stub such as `[native, handle("std::shared_ptr<$T>")] class Weapon;`.
- The storage is `std::weak_ptr<T>`. Each read locks it and gives the strong handle. When the object is gone, the read gives `null`.
- Assign a strong handle to set it. `x = null` clears it. A `null` argument or the default `= null` gives an empty weak handle.
- `Weapon w = x;` copies a strong handle out. A lambda that uses `x` captures the weak handle, and each read in the body locks it.
- A reference into the object is an error, for example `var& r = x.hp`, `&x.hp` or `foreach (var i in x.items)`. The lock ends with the statement, so the reference dangles. Copy the handle to a local first: `Node h = x;`.
- A reload that changes a field from strong to weak, or from weak to strong, resets the field to `null`.

`[weak]` is an error in these places:

| Place | Fix |
| --- | --- |
| An intrusive stub, a value, a builtin type, or a script class with a native base that is not a `std::shared_ptr` | Remove `[weak]`, or give the class the handle `"$std::shared_ptr<$T>"` |
| `[weak] var x`, `[weak] var& x` or a structured binding | Write the handle type: `[weak] T x` |
| A `for` initializer, a `foreach` variable or a lambda parameter | Use a local before the loop or the lambda |
| A method, a stub or a module variable | Put it on a field, a parameter or a local |
| `new C(null)` for a `[weak]` constructor parameter (a known gap: the generated C++ does not compile) | Pass a variable: `Node none = null; new C(none)` |

A `[weak]` parameter does not match a native virtual method with a strong handle parameter. The override then gives the usual "hides" error.

## References and pointers

A parameter `T&` is a reference. The call has no marker: write `f(x)`. A reference is borrowed: use it in the method, but do not store it in a field.

`&x` is the address of `x` and has the type `T*`: write `f(&x)`. `*e` reads through an iterator, an `std.optional`, a smart pointer or a handle. `&h` on a handle is an error: [Native interop](native-interop.html) shows the right forms.

`var& r = x;` and `const var& r = x;` make a reference local. `var r = x;` copies.

```hot
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
        Split(dt, h, calls);                              // no marker
        Make(shot);

        var& r = n;                                       // a reference, not a copy
        r = 2;
        var copy = n;
        std.vector<int> xs = std.vector<int>();
        foreach (var& x in xs) x = x + copy;
        foreach (const var& x in xs) calls += x;
    }
}
```

A `T&` of a script class is a reference to the handle, so the callee can rebind it. A single `T*` of a script class is an error: write `T`. A native handle type keeps the raw `T&` and `T*`.

[Borrowing](borrowing.html) lists every place where a reference or a pointer is allowed.

Not supported: generics in script code, `::`, `->` and brace initializers. `?.` works only at the start of a statement: see [Statements](statements-text.html). Write `T(args)` instead of `T{args}`.
