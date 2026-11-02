---
layout: default
title: Classes
nav_order: 8
group: Hot script
---

A class holds fields and methods. A bare name reaches a member of `this`. This works everywhere: in classes, in structs, before a `.` and for native members of the base class. Write `this.` only to pick the member when a parameter or a local has the same name.

The compiler resolves a bare name in this order:

1. A local or a parameter.
2. A script member: this class first, then the base classes.
3. A native member of the base class. A module variable of the same name wins over it.
4. A module variable, a type, a native global or a namespace.
5. The helper functions `enumerate` and `range`.

## Parts of this guide

This guide has 2 pages. This page covers fields, methods, short bodies, constructors, destructors, properties and structs.

- [Mutual use, this and access words](classes-use.html): classes that use each other, `this` as a value, access words and renamed fields.

## Fields and methods

A field can have an initializer. A method that does not override a native method is a plain script method. An `abstract` method has no body (see [Abstract methods](inheritance.html#abstract-methods)).

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

A default value is an error on a `virtual` or `override` method. A struct constructor cannot have a default for every parameter. Calls from script code and the engine factory use the default value. It is not part of the reload key, so a change of a default value does not need a new key.

## Constructors and destructors

The constructor has the class name. The destructor starts with `~`. `new` calls a constructor and gives a handle. A script class is a handle, so `Helper()` without `new` is an error. Write `new Helper()`.

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

A reload does not run a constructor again. A changed constructor body applies to new objects only. If a constructor body throws, the object is deleted and the destructor still runs. A class has one destructor. Two overloads cannot differ only in the pass mode of a parameter.

The `__` prefix is reserved. Do not start the name of a field or a parameter with `__`.

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

A `struct` is a value type: a copy is a real copy. A `class` is a handle. `[value]` on a class definition is an error: write `struct`. `[value]` is valid on a use of a class (a field, a parameter, a return type or a local). See [Value uses](types-handles.html#value-uses).

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

A script type cannot have the name of a C++ type from a `using` header. This applies to a class, a struct and an enum. The module would bind to the member functions of the host type and corrupt memory. The translator stops with this error:

```
the script type 'Item' has the same name as a C++ type from the header 'Object.h'. Rename the script type.
```
