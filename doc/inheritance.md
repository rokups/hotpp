---
layout: default
title: Inheritance
nav_order: 6
---

A script class can derive from native classes and from other script classes.

## Native base

A method replaces a native virtual method only with `override`. Without it the translator reports an error.

```spp
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

```spp
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

## Script parent

A script class can derive from a script class. `virtual` opens a method for overrides. `base(...)` calls the parent constructor. `new` hides a parent method instead of replacing it.

```spp
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

`using "parent.spp";` makes the classes of another script visible.

```spp
using "parent.spp";
using "Actor.h";

class Grunt : Unit {
    Grunt(int r) : base(r + 1) { }
}
```

For `is` and `as`, see [Statements](statements.html).
