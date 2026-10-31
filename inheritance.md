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
- A method pair `F()` and `F() const` is an error.
- Only the root class of a family can have interface bases. A child class gets them from the root.

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
