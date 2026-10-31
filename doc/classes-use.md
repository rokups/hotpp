---
layout: default
title: Mutual use, this and access words
nav_order: 9
group: Hot script
---

## Classes that use each other

Two classes can hold handles to each other. A class can also hold a handle to its own class. Declare the classes in any order: a handle needs no order.

```hot
class Team {
    Player captain = null;

    int CaptainScore() => captain != null ? captain.score : 0;
}

class Player {
    [weak]
    Team team = null;
    Player rival = null;
    int score = 0;

    void Join(Team t) {
        team = t;
        t.captain = this;
    }
}
```

These are the rules:

- A field, a parameter, a local, a return value and `new` can use a class that comes later in the file.
- A method can use the fields and the methods of every class in the file.
- Two strong handles to each other make a cycle of owners, and neither object is released. Make one handle `[weak]`, or set it to `null`. See [Weak handles](types-handles.html#weak-handles).
- A struct holds a struct by value. Two structs that hold each other have no end size, so the translator stops with an error. Make one of them a class.
- A class that derives from itself through other classes is an error.
- A template argument cannot be a script class yet, for example `std.vector<Player>`. Use a struct, or a field for each handle.
- A known limit: a member access of another class in a field initializer or in a base argument needs that class to come first in the file.

```
the structs use each other in a cycle: Wheel -> Axle -> Wheel. A struct holds the other struct by value, so the size has no end. Make one of them a class: a handle field breaks the cycle
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

Limits: `hotc` checks only known script types. It does not check native members. `hotc` reports all access errors of a class, but it stops after the first class that has an error. The generated C++ has no `private:`, so a hot reload can still move the data.

## Renamed fields

`[alias("old")]` gives the old name of a field. A reload then moves the value to the new name.

```hot
using "Actor.h";

class Player : Actor {
    [alias("hp")]
    float health = 100.0;
}
```
