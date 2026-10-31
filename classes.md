---
layout: default
title: Classes
nav_order: 5
group: Hot script
---

A class holds fields and methods. A bare name reaches a member of `this`. This works everywhere: in classes, in structs, before a `.` and for native members of the base class. Write `this.` only to pick the member when a parameter or a local has the same name.

The compiler resolves a bare name in this order:

1. A local or a parameter.
2. A script member: this class first, then the base classes.
3. A native member of the base class. A module variable of the same name wins over it.
4. A module variable, a type, a native global or a namespace.
5. The helper functions `enumerate` and `range`.

## Fields and methods

A field can have an initializer. A method that does not override a native method is a plain script method.

```hot
class Counter {
    int count = 0;
    std.string label;           // default value

    void Add(int n) { count += n; }
    int Get() { return count; }
    void Set(int count) { this.count = count; }   // this. tells the field from the parameter
}
```

## Short bodies and default values

`=>` gives a method or a read-only property an expression body. A parameter can have a default value. Put it on the first declaration. Constructors and destructors need a block body.

```hot
class Calc {
    int bonus = 2;

    int Double(int x) => x * 2;
    int Bonus => bonus * 10;                 // read-only property
    void Log(int x) => std.printf("%d\n", x);
    int Add(int a, int b = 1, int c = 0) { return a + b + c; }
}
```

A default value is an error on a `virtual` or `override` method. A struct constructor cannot have a default for every parameter.

## Constructors and destructors

The constructor has the class name. The destructor starts with `~`. `new` calls a constructor and gives a handle.

```hot
class Counter {
    int count = 0;

    Counter(int start) { count = start; }
    ~Counter() { std.printf("gone %d\n", count); }
}

class User {
    Counter c = null;

    void Make() {
        c = new Counter(10);
    }
}
```

## Properties

A property has accessors. `get; set;` makes an automatic property. In a custom setter, `value` is the new value.

```hot
using "Actor.h";

class Gauge : Actor {
    float Hp { get; set; } = 50.0;          // automatic
    float MaxHp { get; } = 100.0;           // read only
    int Level { get; private set; } = 1;    // private setter
    float raw = 0.0;
    float Shield {                          // custom accessors
        get { return raw; }
        set { raw = value < 0.0 ? 0.0 : value; }
    }
}
```

## Structs

A `struct` is a value type: a copy is a real copy. A `class` is a handle. `[value]` on a script class is an error: write `struct`.

```hot
struct Point {
    float x = 0.0;
    float y = 0.0;
}

struct Stats {
    float hp = 100.0;
    float Effective() => hp * 2.0;   // bare field in a struct
}

class Holder {
    Point p;
    Stats s = Stats();

    void Copy() {
        Point q = p;            // a copy
        q.x = 1.0;
    }
}
```

## this as a value

`this` is a handle of the current class. Pass it to a handle parameter or store it in a handle local. A `T&` parameter of a script class cannot take `this`: copy it to a local first, as [Borrowing](borrowing.html) shows.

```hot
class Squad {
    Squad leader = null;

    void Join(Squad s) { s.leader = this; }
    void Group() {
        Join(this);
        var me = this;               // a Squad handle
        leader = me;
    }
}
```

## Access words

`public`, `protected` and `private` work on fields, methods, properties, accessors and constructors. The default is `public`. `private` is for the class. `protected` is for the class and its script subclasses. A subclass `D` uses a protected member of its parent only through `this` or a `D` object, not through a parent object or a sibling subclass object. A bad use gives `'X' is private in 'C'`.

```hot
class Account {
    private int balance = 0;
    protected int Fee() => 2;
    public void Pay(int n) { balance -= n + Fee(); }
}

class Vip : Account {
    int Cheap() => Fee() - 1;
}
```

```hot
// error
class Account { private int balance = 0; }

class Spy {
    void Peek(Account& a) { var b = a.balance; }   // 'balance' is private in 'Account'
}
```

Limits: hotc checks only known script types. It does not check native members. hotc reports all access errors of a class, but it stops after the first class that has an error. The generated C++ has no `private:`, so a hot reload can still move the data.

## Renamed fields

`[alias("old")]` gives the old name of a field. A reload then moves the value to the new name.

```hot
using "Actor.h";

class Player : Actor {
    [alias("hp")]
    float health = 100.0;
}
```
