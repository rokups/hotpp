---
layout: default
title: Handles, references and pointers
nav_order: 5
group: Hot script
---

## Handles

`$C` is a handle of the script class `C`. `new $C(...)` makes the object and `null` clears the handle. A bare `C` is a value: see [Value uses](#value-uses). A native class needs a stub to be a handle.

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
    $Child kid = null;
    int hp = 10;

    void Adopt($Child c) {
        if (c == null) return;
        kid = c;
        c.parent = this;
    }
}

class Child {
    [weak]
    $Parent parent = null;

    int ParentHp() { return parent != null ? parent.hp : 0; }
    void See([weak] $Parent p) { [weak] $Parent seen = p; }
}
```

The two classes use each other, and the order in the file does not matter. See [Classes that use each other](classes-use.html#classes-that-use-each-other).

These are the rules:

- The type is a handle with a `std::shared_ptr` handle template. A script class without a native base is one. A native class needs a stub such as `[native, handle("std::shared_ptr<$T>")] class Weapon;`.
- The storage is `std::weak_ptr<T>`. Each read locks it and gives the strong handle. When the object is gone, the read gives `null`.
- A stub with another handle template can name its weak template with `weak_handle` (see "Custom handles" below).
- Assign a strong handle to set it. `x = null` clears it. A `null` argument or the default `= null` gives an empty weak handle.
- `Weapon w = x;` copies a strong handle out. A lambda that uses `x` captures the weak handle, and each read in the body locks it.
- A reference into the object is an error, for example `ref r = x.hp` or `foreach (var i in x.items)`. The lock ends with the statement, so the reference dangles. Copy the handle to a local first: `Node h = x;`.
- A weak handle takes a strong handle, `this`, another weak handle, `null` or the result of a call. It cannot take a `new` object: see "A new object in a weak handle" below.
- A reload that changes a field from strong to weak, or from weak to strong, resets the field to `null`.

`[weak]` is an error in these places:

| Place | Fix |
| --- | --- |
| An intrusive stub, a value, a builtin type, or a script class with a native base that is not a `std::shared_ptr` | Remove `[weak]`, or give the class the handle `"$std::shared_ptr<$T>"` |
| `[weak] var x`, `[weak] ref x` or a structured binding | Write the handle type: `[weak] T x` |
| A `for` initializer, a `foreach` variable or a lambda parameter | Use a local before the loop or the lambda |
| A method, a stub or a module variable | Put it on a field, a parameter or a local |
| `new C(null)` for a `[weak]` constructor parameter (a known gap: the generated C++ does not compile) | Pass a variable: `$Node none = null; new $C(none)` |

#### A new object in a weak handle

A new object has no strong handle yet. A weak handle does not keep it alive, so the object dies at once and the handle is `null`. The `.hot` frontend reports an error when `new` is the whole value of a weak handle. The error text is `'new' cannot be stored in a weak handle: the new object dies at once and the handle is null.` The error is for these places:

- the initializer of a `[weak]` field, local or parameter default: `[weak] $Node w = new $Node();`
- an assignment to a weak handle: `parent = new $Node();`, `this.parent = new $Node();` or `k.parent = new $Node();`
- an argument for a `[weak]` parameter of a function, a method or a constructor: `Take(new $Node());`

Parentheses around `new` do not hide it. Keep the object in a strong handle first, then give that handle to the weak one:

```hot
$Node n = new $Node();
parent = n;
```

The check sees only a direct `new`. A `new` inside a longer expression, for example `c ? new $Node() : n`, is not checked. A function returns a strong handle, so no function returns a weak handle. The `.cpp` frontend does not check this: it keeps its minimal rewriting.

A `[weak]` parameter does not match a native virtual method with a strong handle parameter. The override then gives the usual "hides" error.

### Custom handles

A host can use its own smart pointer, for example an intrusive pointer of an engine. The stub gives the handle template and the weak template:

```
[native, handle("Urho3D::SharedPtr<$T>"), weak_handle("Urho3D::WeakPtr<$T>")] class Urho3D.RefCounted;
```

- The rule is for the class and for all its subclasses. A script class with such a native base uses the same handle.
- `new T(args)` gives `H(new T(args))`. The handle must take the ownership of a new raw pointer, as an intrusive pointer does.
- `weak_handle` is valid on a stub with a `handle` other than `"$T*"`, and on `class *` with a `default_handle`. The template must hold `$T`.
- `factory("std::make_shared<$T>")` sets the callee of `new $T(args)` on a native class. `$T` is the class. The text `(args)` follows the template. An intrusive stub with no `factory` emits `H(new T(args))`. A `std::shared_ptr` handle with no `factory` emits `std::make_shared<T>(args)`.
- A `$T` cannot come from a raw pointer. This applies to `this`, to a `T*` value and to `h.reset(p)`. The error text is `cannot create a shared pointer from a raw pointer; use new $T(...)`. The exception is a class with a weak source: a script class, or a native class with `std::enable_shared_from_this`. The form `h.reset(p)` is always an error.
- `$Derived` converts to `$Base`. The reverse needs a cast.
- A read of a `[weak]` value calls `lock() or `Lock()` of the weak pointer.
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

A bare script class name `C` is a value. It holds its own object, and a copy is a new object. `$C` is a handle. A copy of a handle names the same object. See the table in [Type design](type-design.html).

For a type that is always a value, write a `struct` (see [Classes](classes.html#structs)).

A value is valid on a field, a parameter, the return type of a method, a local, a foreach variable and a lambda parameter. A bare `C` in a template argument is a value too: `std.vector<C>` holds values. A class with a native base cannot be a value in any of these places.

`as` and `is` need the handle: write `x as $C` and `x is $C`. A cast `($C)h` to a derived class is a checked cast. A failed cast gives a null handle. A foreach over `std.vector<$C>` needs the variable `$C x`.

```hot
using "Actor.h";

class Pt {
    int x = 1;
}

class Holder : Actor {
    Pt a;                                   // a field: new Pt() when you give no initializer

    int Bump(Pt p) {                        // a parameter: p is a copy
        p.x = p.x + 1;
        return p.x;
    }

    Pt Make(int v) {                        // a return type: the caller gets its own object
        Pt t = new Pt();                    // a local
        t.x = v;
        return t;
    }
}
```

### Storage

A `Pt` is stored as `hot::Value<Pt>`. This is a box that owns one object. The box is never null. A copy of the box copies the object with its real class. So a `Sub` that a `Pt` holds stays a `Sub` after the copy.

A value is valid for a script class with no native base in its chain. The class can have script base classes. A script class with a native base is always a handle: write `$C` and `new $C(...)`.

### The new forms

| Text | Result |
| --- | --- |
| `new C(args)` on a script class | A value: the object goes into a `hot::Value<C>` |
| `new $C(args)` on a script class | A handle |
| `new C(args)` on a struct or a native class | The value or the object of the native rule |
| `new $S(args)` on a struct | A `std::shared_ptr<S>` |

### Copy rules

- `new Pt()` is not copied. The new object goes into the box.
- A source that is a handle is copied. `Pt hv = h;` makes a new object, and `hv.x = 43;` does not change `h`. When `h` is `null`, the copy throws a null reference error.
- A source that is a value is copied: `this.b = this.a;` gives `b` its own object.
- A parameter copies its argument. In `Bump(this.a)`, the callee changes the copy, and `this.a` stays the same.
- A return copies the returned value. `Pt Dup() { return this.a; }` gives a copy.
- `Pt f;` with no initializer runs `new Pt()`. The class needs a constructor with no parameters, or a constructor where all parameters have defaults. Else write an initializer.
- `var x = valueExpr;` makes a value. In `var m = this.a;`, `m` is a copy, and `m.x = 1000;` does not change `this.a`.
- A lambda that uses a value local captures a copy. A later change to the local is not visible in the lambda.
- A value is never null. Its object is not shared with a handle.

```hot
Pt cap = new Pt();
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
| A script class with a native base, for example `Boss b;` or `new Boss()` | `a script class with a native base cannot be a value; write new $C(...)` |
| `Pt p = null;`, an assignment of `null` or `return null;` for a value | `a value cannot be null; write $C w` |
| `var x = null;` | `null has no type; write the type` |
| `Cfg c;` when `Cfg` has only a constructor with parameters | `a value of the class 'Cfg' in field 'c' has no initializer, so the class needs a constructor with no parameters. Add one, or write an initializer` |
| `ref Pt p` or `out Pt p` | `a reference to a value of a script class is not supported; write $Pt` |
| A value on a module variable | `a module variable cannot hold a value of a script class; write $Pt g` |
| A value field of a struct | `the field 'p' of the struct 'Wrap' cannot hold a value of a script class; write $Pt p` |
| `a == b` or `a != b` where one side is a value and the class has no `operator==`, also `a == null` | `'==' is not valid for a value of a script class: it is never null and it has no identity` |
| `$Pt h = a;` where `a` is a value | `a value cannot convert to a handle: the box owns the object. Declare the target without '$' to copy it` |

To compare two values, write `operator==` in the class (see [Operators](classes.html#operators)), or compare their fields. A struct has a generated field-wise `==` and `!=`. A written `operator==` replaces it. An operator other than `==` and `!=` is an error on a handle. To pass the object to a handle, declare the target as a value.

An `override` must have the same parameter types as the virtual method. `$Pt` and `Pt` are two types. Else the translator reports: `'F' has other parameter types than the virtual method of Base; write 'new' to hide it`.

### Reload

A reload migrates the object inside the box as a live object. The loader moves it and does not copy it. The usual rules for fields apply to the object: see [Hot reload](hot-reload.html). A value field that you add gets its initializer, as any new field does.

## References and pointers

A parameter `ref T x` or `out T x` is a reference. A parameter `in T x` is a const reference. The call has no marker: write `f(x)`. A reference is borrowed: use it in the method, but do not store it in a field.

An `out T x` parameter must have a value on every path. The body assigns it before each `return` and before the end of the body. A loop can run zero times, so an assignment in a loop does not count after the loop. The error is `out parameter "x" is not assigned on every path`. A read before the first assignment gives `out parameter "x" is read before it is assigned`. A parameter `[byval] T x` is a copy. `T& x` is an error: write `ref T x`.

There is no prefix `&` (address-of) in `.hot`. Pass a value lvalue `x` to a native `T*` parameter as `f(x)`: the call writes `hot::as_ptr(x)`. `*e` reads through an iterator, an `std.optional`, a smart pointer or a handle. [Native interop](native-interop.html) shows the forms for a handle.

`ref r = x;` makes a reference local. `var r = x;` copies.

```hot
using "Actor.h";

class Bullet {
    int dmg = 1;
}

class Mover : Actor {
    $Bullet shot = null;
    int n = 0;

    void Split(float total, ref float half, ref int calls) {
        half = total / 2.0;
        calls++;
    }

    void Make(ref $Bullet b) { b = new $Bullet(); }            // b is the handle itself

    override void Tick(float dt) {
        float h = 0.0;
        int calls = 0;
        Split(dt, h, calls);                              // no marker
        Make(shot);

        ref r = n;                                       // a reference, not a copy
        r = 2;
        var copy = n;
        std.vector<int> xs = std.vector<int>();
        foreach (ref x in xs) x = x + copy;
        foreach (ref x in xs) calls += x;
    }
}
```

A `ref $C` parameter is a reference to the handle, so the callee can rebind it. A single `C*` of a script class is an error: write `$C`. A native handle type keeps the raw `T&` and `T*`.

### Pointer fields

A field or a module variable `T*` of a native class is stored as `hot::CheckedPtr<T>`. It is a weak pointer. It does not keep the object alive and it reads `null` after the object dies. It keeps its target across a reload. `p.M()` and `*p` throw `NullReference` on a null or dead pointer.

`T` needs a weak source:

| Weak source | Raw `T*` assignment | Handle assignment |
|---|---|---|
| A class with `std::enable_shared_from_this` | yes (`NullReference` if no `shared_ptr` owns the object) | yes |
| A stub with `handle("std::shared_ptr<$T>")` and no `enable_shared_from_this` | no | yes |
| A stub with `handle` and `weak_handle` (see "Custom handles") | no | yes |
| A `hot::weak_ref_traits<T>` specialization of the host | only with `from_raw` | yes |

A `T*` of an intrusive stub stays the handle. A pointer to a class with no weak source is a borrowed finding. A reference field, a pointer to a builtin or enum, and `[weak]` on a pointer field are errors. See "Pointer fields" in [Borrowing](borrowing.html) for the error texts.

[Borrowing](borrowing.html) lists every place where a reference or a pointer is allowed.

Not supported: generics in script code, `::`, `->` and brace initializers. Write `T(args)` instead of `T{args}`.
