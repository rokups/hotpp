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
- A stub with another handle template can name its weak template with `weak_handle` (see "Custom handles" below).
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

### Custom handles

A host can use its own smart pointer, for example an intrusive pointer of an engine. The stub gives the handle template and the weak template:

```
[native, handle("Urho3D::SharedPtr<$T>"), weak_handle("Urho3D::WeakPtr<$T>")] class Urho3D.RefCounted;
```

- The rule is for the class and for all its subclasses. A script class with such a native base uses the same handle.
- `new T(args)` gives `H(new T(args))`. The handle must take the ownership of a new raw pointer, as an intrusive pointer does.
- `weak_handle` is valid on a stub with a `handle` other than `"$T*"`, and on `class *` with a `default_handle`. The template must hold `$T`.
- A read of a `[weak]` value calls `lock()` or `Lock()` of the weak pointer.
- The handle needs `operator->`, `operator*` and a test against `nullptr`. The raw pointer comes from `get()` or `Get()`.
- For a template of the form `Name<$T>`, the module header specializes `hot::is_handle`. For another form, the host specializes `hot::is_handle` in a header that the script includes.

A stub with a non-intrusive handle is also a weak source for pointer fields (see "Pointer fields" below). The module header writes a `hot::weak_ref_traits` specialization for each such stub. The specialization is guarded: it does not apply to a class with `weak_from_this()`. A host can write the specialization by hand for a smart pointer that the stub does not describe:

```cpp
template <> struct hot::weak_ref_traits<MyType> {
    using strong_type = MySharedPtr<MyType>;
    using weak_type = MyWeakPtr<MyType>;
    static weak_type to_weak(const strong_type& s);   // makes the weak handle
    static strong_type lock(const weak_type& w);      // null handle when the object is gone
    static strong_type from_raw(MyType* p);           // optional: a raw pointer to a handle
};
```

`hot::custom_weak_ref<T, Strong, Weak>` and `hot::shared_weak_ref<T>` are ready bases. They give `to_weak` and `lock`, and no `from_raw`. Without `from_raw`, a pointer field takes a handle only, not a raw `T*`.

All modules must use the same handle stub for one class. Each module header has its own `weak_ref_traits` for the class. Two different stubs for one class break the one-definition rule.

With a module attribute `std` other than `std`, a script class without a native base uses `$std::shared_ptr`. Then `new` makes the object and the control block in two blocks. `this` as a handle in such a class is not supported: `hot::PlainObject` has `std::enable_shared_from_this`.

## Value uses

A script class is a handle. A copy of a handle names the same object. `[value]` on one use of the type gives value semantics: the use holds its own object, and a copy is a new object.

`[value]` goes on a use of a type, not on the class. A class definition cannot take it: `[value] class V { }` is an error. For a type that is always a value, write a `struct` (see [Classes](classes.html#structs)).

Write `[value]` on a field, a parameter, the return type of a method or a local. On a field and on a method, write the attribute on its own line. On a parameter and a local, write it inline.

```hot
using "Actor.h";

class Pt {
    int x = 1;
}

class Holder : Actor {
    [value]
    Pt a;                                   // a field: new Pt() when you give no initializer

    int Bump([value] Pt p) {                // a parameter: p is a copy
        p.x = p.x + 1;
        return p.x;
    }

    [value]
    Pt Make(int v) {                        // a return type: the caller gets its own object
        [value] Pt t = new Pt();            // a local
        t.x = v;
        return t;
    }
}
```

### Storage

A `[value] Pt` is stored as `hot::Value<Pt>`. This is a box that owns one object. The box is never null. A copy of the box copies the object with its real class. So a `Sub` that a `[value] Pt` holds stays a `Sub` after the copy.

`[value]` is valid for a script class with no native base in its chain. The class can have script base classes.

### Copy rules

- `new Pt()` is not copied. The new object goes into the box.
- A source that is a handle is copied. `[value] Pt hv = h;` makes a new object, and `hv.x = 43;` does not change `h`. When `h` is `null`, the copy throws a null reference error.
- A source that is a `[value]` is copied: `this.b = this.a;` gives `b` its own object.
- A parameter copies its argument. In `Bump(this.a)`, the callee changes the copy, and `this.a` stays the same.
- A return copies the returned `[value]`. `[value] Pt Dup() { return this.a; }` gives a copy.
- `[value] Pt f;` with no initializer runs `new Pt()`. The class needs a constructor with no parameters, or a constructor where all parameters have defaults. Else write an initializer.
- `var x = valueExpr;` makes a `[value]`. In `var m = this.a;`, `m` is a copy, and `m.x = 1000;` does not change `this.a`.
- A lambda that uses a `[value]` local captures a copy. A later change to the local is not visible in the lambda.
- A `[value]` is never null. Its object is not shared with a handle.

```hot
[value] Pt cap = new Pt();
cap.x = 3;
var f = () => cap.x;     // the lambda has its own copy
cap.x = 5;               // f() still gives 3
```

A copy needs a copyable object. When a field of the class cannot be copied, the copy throws a run-time error with this text:

```
class X is not copyable: field 'f'
```

### Errors

These uses are errors. The text in quotes is the message of the translator.

| Use | Message or rule |
| --- | --- |
| `[weak, value]` | `[weak] and [value] cannot be used together: a weak handle is not a value` |
| A native class with a handle stub, for example `[value] Weapon w;` | `[value] on field 'w' is not valid for the native type 'Weapon'. The type rules decide the storage of a native type. Remove '[value]'` |
| A script class with a native base, for example `[value] Boss b;` | `[value] on field 'b' needs a script class with no native base, and 'Boss' has the native base 'Widget'. Remove '[value]'` |
| A struct or a builtin, for example `[value] Stats s;` or `[value] int n;` | `[value] on field 's' has no effect: 'Stats' is a value type already. Remove '[value]'` |
| `[value] Cfg c;` when `Cfg` has only a constructor with parameters | `[value] on field 'c' has no initializer, so the class 'Cfg' needs a constructor with no parameters. Add one, or write an initializer` |
| `[value] Pt& p` or `[value] const Pt& p` | `[value] on a reference is not valid: a [value] holds its own copy. Remove the reference or the '[value]'` |
| A `[value]` on a module variable | `[value] is not supported yet on a module variable` |
| A `[value]` field of a struct | `[value] on field 'p' of the struct 'Wrap' is not supported yet` |
| A lambda parameter | `attribute 'value' is not valid on a lambda parameter` |
| A property, for example `[value] int p { get; set; }` | `[value] applies to fields, parameters, local variables and the return type of a method only` |
| `= null` on a `[value]` | `a [value] is never null: it always holds an object` |
| `a == b` or `a != b` where one side is a `[value]`, also `a == null` | `'==' is not valid for a [value]: a [value] is never null and it has no identity` |
| `Pt h = a;` where `a` is a `[value]` | `a [value] cannot convert to a handle: the object is owned by the [value]. Declare the target as a [value] to copy it` |
| A `[value]` as a template argument, for example in `std.vector<...>` | Not supported in this version |

To compare two values, compare their fields. To pass the object to a handle, declare the target as a `[value]`.

An `override` must have the same `[value]` as the virtual method on each parameter and on the return type. Else the translator reports: `'F' differs in '[value]' on the parameter 'p' from the virtual method of Base; write '[value]' on both or on none`.

### Reload

A reload migrates the object inside the box as a live object. The loader moves it and does not copy it. The usual rules for fields apply to the object: see [Hot reload](hot-reload.html). A `[value]` field that you add gets its initializer, as any new field does.

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

### Pointer fields

A field or a module variable `T*` or `const T*` of a native class is stored as `hot::CheckedPtr<T>`. It is a weak pointer. It does not keep the object alive and it reads `null` after the object dies. It keeps its target across a reload. `p.M()` and `*p` throw `NullReference` on a null or dead pointer.

`T` needs a weak source:

| Weak source | Raw `T*` assignment | Handle assignment |
|---|---|---|
| A class with `std::enable_shared_from_this` | yes (`NullReference` if no `shared_ptr` owns the object) | yes |
| A stub with `handle("std::shared_ptr<$T>")` and no `enable_shared_from_this` | no | yes |
| A stub with `handle` and `weak_handle` (see "Custom handles") | no | yes |
| A `hot::weak_ref_traits<T>` specialization of the host | only with `from_raw` | yes |

A `T*` of an intrusive stub stays the handle. A pointer to a class with no weak source is a borrowed finding. A reference field, a pointer to a builtin or enum, and `[weak]` on a pointer field are errors. See "Pointer fields" in [Borrowing](borrowing.html) for the error texts.

[Borrowing](borrowing.html) lists every place where a reference or a pointer is allowed.

Not supported: generics in script code, `::`, `->` and brace initializers. `?.` works only at the start of a statement: see [Statements](statements-text.html). Write `T(args)` instead of `T{args}`.
