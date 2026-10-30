---
layout: default
title: Statements
nav_order: 8
---

Statements look like C#, with C++ names and `.` for scope.

## Control flow

`if`, `for`, `while`, `do` and `?:` work as in C++.

```hot
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

`switch` works as in C++ for enums and integers. A `switch` on string labels becomes an `if` chain. Each case must end with `break`, `return`, `throw` or `continue`. The last case can end any way. A case can have several labels. Mixed labels and repeated labels are errors.

```hot
enum Mode { Idle, Walk, Run }

class Speed {
    int Of(Mode m) {
        switch (m) {
            case Mode.Idle: return 0;
            case Mode.Walk: return 1;
            default: return 10;
        }
    }

    int Word(std.string s) {
        switch (s) {
            case "idle": return 0;
            case "walk":
            case "run": return 1;
            default: return 10;
        }
    }
}
```

## foreach

`foreach` walks a container. `var x` and `T x` copy each element. `var& x` and `const var& x` bind a reference to the element.

```hot
using <map>;

class Sum {
    int Total(std.vector<int>& xs) {
        int sum = 0;
        foreach (var x in xs) sum += x;
        foreach (const int y in xs) sum += y;
        foreach (const var& z in xs) sum += z;     // no copy
        return sum;
    }

    void Double(std.vector<int>& xs) {
        foreach (var& x in xs) x = x * 2;          // changes the element
    }

    int Pairs(std.map<std.string, int>& m, std.vector<int>& xs) {
        int sum = 0;
        foreach (var (k, v) in m) sum += v;                        // key and value
        foreach (var (i, x) in enumerate(xs)) sum += i * x;   // index and element
        foreach (int i in range(3)) sum += i;                  // 0, 1, 2
        foreach (int i in range(2, 5)) sum += i;               // 2, 3, 4
        return sum;
    }
}
```

`enumerate` and `range` are global functions. A local, a member, a type or a native global of the same name hides them. In `enumerate`, `i` is a copy and `x` is a reference to the element. `var&` over `enumerate` is an error.

## Structured bindings

`var (a, b) = e;` names the parts of a `std.pair`, a `std.tuple`, a struct or a map entry. It needs two names or more. `var&` binds references. `const var` is also allowed.

```hot
class Split {
    std.pair<int, float> pair = std.make_pair(3, 4.5);

    std.pair<int, float> Make() => std.make_pair(1, 2.5);

    void Run() {
        var (a, b) = Make();       // copies
        var& (c, d) = pair;        // references into the field
        c = a + 1;
    }
}
```

## try, catch, finally

You throw and catch C++ exception types. A `finally` block always runs. A script cannot declare an exception type.

```hot
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

```hot
using "Actor.h";

class Hud : Actor {
    float hp = 12.3456;

    override void Tick(float dt) {
        Name = "hp = ${hp}";
        Name = "hp = ${hp:.2f}";
        Name = "${hp:>8.1f} at ${dt * 2}";
    }
}
```

## Casts

A C style cast `(T)e` works for a builtin type (`(int)f`) and for a name with `*` or `&` (`(Foo*)p`). For any other type, use `static_cast<T>(e)`.

```hot
class Caster {
    void Run(float f) {
        int i = static_cast<int>(f * 100.0);
        float g = static_cast<float>(i);
        var h = (float)i;                // C style cast
        var k = int(f);                  // function style
    }
}
```

For a checked cast to a script class, use `as` (see "is and as").

## Null coalescing and safe calls

`a ?? b` gives `a` when `a` is not null (or not false). Otherwise it gives `b`. The right side runs only when it is needed. `??` binds between `||` and `?:`. It is right associative.

`?.` works only at the start of an expression statement: `h?.F();` and `h?.x = 1;` do nothing when `h` is null.

```hot
class Pick {
    Pick next = null;
    int x = 0;
    void F() { }

    Pick Choose(Pick a, Pick b, Pick c) {
        var p = a ?? b ?? c;                  // a ?? (b ?? c)
        p?.F();
        p?.x = 1;
        return p;
    }
}
```

## is and as

`x is T` tests the type of a handle. `x as T` gives a `T` handle, or `null` when the type does not match. `x is T name` also binds a `T` handle. It works only as the first part of an `if` or `while` condition. `&&` can follow it.

```hot
class Unit {
    int hp = 100;
    virtual int Power() { return 1; }
}

class Grunt : Unit {
    int rage = 3;
    override int Power() { return rage; }
}

class Spotter {
    Unit seen = null;

    void Look() {
        if (seen == null) seen = new Grunt();
        bool isGrunt = seen is Grunt;
        var g = seen as Grunt;
        if (g != null) seen.hp = g.rage;
        if (seen is Grunt h && h.rage > 1) seen.hp = h.rage;
    }
}
```

`is` and `as` work after a reload. An object that a generation made keeps its class after the reload, and `is` still finds it. The test uses the name of the script class. It does not use C++ RTTI, so a cast of an object of an old generation also works.

A `.cpp` script has the same rules with the C++ forms (see [C++ scripts](cpp-scripts.html#script-classes-as-types)):

| `.hot` | `.cpp` | Difference |
|---|---|---|
| `x is T` | `dynamic_cast<T*>(x) != nullptr` | None. |
| `x as T` | `dynamic_cast<T*>(x)` | `as` gives a handle. `dynamic_cast` gives a raw pointer. |
| None | `dynamic_cast<T&>(x)` | It throws `std::runtime_error` when the type does not match. |
| `new T()` | `new T()` | `.hot` gives a handle. `.cpp` gives a raw pointer, and the script code must `delete` the object. |
| `T` (a handle) | `T*` or `T&` | A `.cpp` script cannot hold a script object by value. |
| None | `typeid(T)` | An error in a `.cpp` script. Use `dynamic_cast`. |
