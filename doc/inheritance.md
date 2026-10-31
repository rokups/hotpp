---
layout: default
title: Inheritance
nav_order: 7
group: Hot script
---

A script class can derive from native classes and from other script classes.

## Native base

A method replaces a native virtual method only with `override`. Without it the translator reports an error.

```hot
using "Actor.h";

class Player : Actor {
    override void Tick(float dt) {
        Move(Vec3(dt, 0, 0));
    }
    override void TakeDamage(float amount) { }
}
```

## Several native bases

The first base is the main base. The other bases are interfaces. Implement each pure virtual method with `override`. `base.` calls the interface code.

```hot
using "Actor.h";
using "Damageable.h";

class Boss : Actor, IDamageable {
    int hp = 50;

    override int Absorb(int amount) {
        hp = hp - amount;
        return hp;
    }
    int BaseArmor() { return base.Armor(); }
}
```

These rules apply to the bases:

- The first base gets the constructor arguments.
- Each other base needs a default constructor.
- A signature in two bases is an error.
- A method pair `F()` and `F() const` needs a marker. See below.
- Only the root class of a family can have interface bases. A child class gets them from the root.

### Const methods

Two native bases can have `F()` and `F() const` as virtual methods. Write `const` after the parameters to select the const method.

- Without `const`, the override matches by name and parameters. If both methods match, the non-const method wins.
- A native base with only a const method matches an override without `const`.
- `const` on an override whose native method is not const is an error.

```hot
class Foo : Widget, IConst {
    // Widget has `Size()` and IConst has `Size() const`.
    override int Size() const => 2; // overrides the const method of IConst
}
```

A script class has one method for each name. The marker only selects the native method. The body can still write the fields of `this`.

### Parameter forms of an override

A native virtual method can take a handle type in several forms. Write the plain handle type in the override. The translator finds the native method and keeps its signature. The body gets a handle.

| Native parameter | The body gets |
|---|---|
| `Weapon*`, `const Weapon*` | `Weapon` through `shared_from_this()`, or an intrusive reference |
| `Weapon&`, `const Weapon&` | the same |
| `std::shared_ptr<Weapon>`, `const std::shared_ptr<Weapon>&`, `std::shared_ptr<Weapon>&&` | the handle itself |

```hot
class Foo : Gun {
    // Gun has `virtual void Hit(Weapon* w)`.
    override void Hit(Weapon w) { w.dmg = 1; }
}
```

- The exact native spelling always works and wins. `override void Hit(Weapon* w)` is the exact form.
- If two native methods carry the same handle type, the override is ambiguous. The error lists each method. Write the exact parameter type.
- If no native method matches, the error `hides the virtual method` lists the candidates. A `.cpp` script also gets a hint with the native parameter type.
- A `std::shared_ptr` stub needs `std::enable_shared_from_this` in the native class to take a `T*` or a `T&`. Without it, the C++ compile stops with a message. Write the exact type instead.
- A `const Weapon*` or `const Weapon&` binds a normal handle. A script handle has no `const`. The handle gives write access even when the native method passes `const`. Do not write to it.
- An intrusive stub (`intrusive("AddRef", "Release")`) adds a reference for a `T*`.
- A `T*` of an object that no `std::shared_ptr` owns throws `NullReference` in the override. A null `T*` gives a null handle.
- A `.cpp` script does not use this rule. It writes the exact native type.
- A native method that returns `std::shared_ptr<Weapon>` by value can be overridden with the return type `Weapon`. The slot keeps the native return type.
- A native return type `Weapon*`, `Weapon&` or an intrusive `Weapon*` is not relaxed: the lifetime of the result is not clear. Write the exact native type.
- A borrowed `T*` or `T&` in the body is not part of this rule.

## Script parent

A script class can derive from a script class. `virtual` opens a method for overrides. `base(...)` calls the parent constructor. `new` hides a parent method instead of replacing it.

```hot
using "Actor.h";

class Unit : Actor {
    int hp = 100;

    Unit(int start) : base(std.string("unit")) { hp = start; }
    virtual int Power() { return 1; }
    int Attack() { return Power() * 10; }
}

class Grunt : Unit {
    int rage = 3;

    Grunt(int start, int r) : base(start + 1) { rage = r; }
    override int Power() { return rage * 2; }
    new int Attack() { return base.Attack() + 1; }
}
```

## Parent in another file

`using "parent.hot";` makes the classes of another script visible. The script joins the generation of the child. A class name in two scripts is an error. A native stub in the main script wins over a stub with the same name in the imported script.

```hot
using "parent.hot";
using "Actor.h";

class Grunt : Unit {
    Grunt(int r) : base(r + 1) { }
}
```

For `is` and `as`, see [Statements](statements.html).
