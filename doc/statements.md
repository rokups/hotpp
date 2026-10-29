---
layout: default
title: Statements
nav_order: 7
---

Statements look like C#, with C++ names and `.` for scope.

## Control flow

`if`, `for`, `while`, `do` and `?:` work as in C++.

```csharp
class Flow {
    int Run(int n) {
        int sum = 0;
        if (n > 10) {
            sum = 1;
        } else if (n > 5) {
            sum = 2;
        }
        for (int i = 0; i < n; i++) sum += i;
        while (sum > 100) sum -= 10;
        do { sum++; } while (sum < 5);
        return n > 0 ? sum : -sum;
    }
}
```

## switch

`switch` works as in C++. Use it with enums and integers.

```csharp
enum Mode { Idle, Walk, Run }

class Speed {
    int Of(Mode m) {
        switch (m) {
            case Mode.Idle: return 0;
            case Mode.Walk: return 1;
            default: return 10;
        }
    }
}
```

## foreach

`foreach` walks a container. Use `var` or a type, with or without `const`.

```csharp
using <vector>;

class Sum {
    int Total(std.vector<int>& xs) {
        int sum = 0;
        foreach (var x in xs) sum += x;
        foreach (const int y in xs) sum += y;
        return sum;
    }
}
```

## try, catch, finally

You throw and catch C++ exception types. A `finally` block always runs. A script cannot declare an exception type.

```csharp
using <stdexcept>;

class Safe {
    int Div(int x, int y) {
        try {
            if (y == 0) throw std.runtime_error("divide by zero");
            return x / y;
        } catch (const std.exception& e) {
            std.printf("error: %s\n", e.what());
            return 0;
        } catch (...) {
            throw;
        } finally {
            std.printf("done\n");
        }
    }
}
```

## String interpolation

`${expr}` puts a value in a string. Add `:` and a format spec after the expression.

```csharp
using "Actor.h";
using <string>;

class Hud : Actor {
    float hp = 12.3456;

    override void Tick(float dt) {
        this.Name = "hp = ${this.hp}";
        this.Name = "hp = ${this.hp:.2f}";
        this.Name = "${this.hp:>8.1f} at ${dt * 2}";
    }
}
```

## Casts

Use the C++ cast operators. A C style cast `(int)x` is not supported.

```csharp
class Caster {
    void Run(float f) {
        int i = static_cast<int>(f * 100.0);
        float g = static_cast<float>(i);
        var k = int(f);                  // function style
    }
}
```

## is and as

`x is T` tests the type of a handle. `x as T` gives a `T` handle, or `null` when the type does not match.

```csharp
class Unit {
    int hp = 100;
    virtual int Power() { return 1; }
}

class Grunt : Unit {
    int rage = 3;
    override int Power() { return this.rage; }
}

class Spotter {
    Unit seen = null;

    void Look() {
        if (this.seen == null) this.seen = new Grunt();
        bool isGrunt = this.seen is Grunt;
        var g = this.seen as Grunt;
        if (g != null) this.seen.hp = g.rage;
    }
}
```
