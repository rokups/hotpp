---
layout: default
title: Classes
nav_order: 4
---

A class holds fields and methods. Use `this.` to reach a member.

## Fields and methods

A field can have an initializer. A method that does not override a native method is a plain script method.

```csharp
class Counter {
    int count = 0;
    std.string label;           // default value

    void Add(int n) { this.count += n; }
    int Get() { return this.count; }
}
```

## Constructors and destructors

The constructor has the class name. The destructor starts with `~`. `new` calls a constructor and gives a handle.

```csharp
class Counter {
    int count = 0;

    Counter(int start) { this.count = start; }
    ~Counter() { std.printf("gone %d\n", this.count); }
}

class User {
    Counter c = null;

    void Make() {
        this.c = new Counter(10);
    }
}
```

## Properties

A property has accessors. `get; set;` makes an automatic property. In a custom setter, `value` is the new value.

```csharp
using "Actor.h";

class Gauge : Actor {
    float Hp { get; set; } = 50.0;          // automatic
    float MaxHp { get; } = 100.0;           // read only
    int Level { get; private set; } = 1;    // private setter
    float raw = 0.0;
    float Shield {                          // custom accessors
        get { return this.raw; }
        set { this.raw = value < 0.0 ? 0.0 : value; }
    }
}
```

## Structs

A `struct` is a value type: a copy is a real copy. `[value] class` means the same. Other classes are handles.

```csharp
struct Point {
    float x = 0.0;
    float y = 0.0;
}

[value] class Stats {
    float hp = 100.0;
    float Effective() { return this.hp * 2.0; }
}

class Holder {
    Point p;
    Stats s = Stats();

    void Copy() {
        Point q = this.p;       // a copy
        q.x = 1.0;
    }
}
```

## Renamed fields

`[alias("old")]` gives the old name of a field. A reload then moves the value to the new name.

```csharp
using "Actor.h";

class Player : Actor {
    [alias("hp")]
    float health = 100.0;
}
```
