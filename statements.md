---
layout: default
title: Statements
nav_order: 12
group: Hot script
---

Statements look like C#, with C++ names and `.` for scope.

## Parts of this guide

This guide has 3 pages. This page covers control flow, `switch`, `foreach` and structured bindings.

- [try, catch and runtime errors](statements-errors.html): `try`, `catch`, `finally` and the errors that the runtime throws.
- [Strings, casts and patterns](statements-text.html): string interpolation, casts, `??`, `?.`, `is` and `as`.

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

`switch` works as in C++ for enums and integers. A `switch` on string labels becomes an `if` chain. Each case must end with `break`, `return`, `throw` or `continue`. The last case can end any way. A case can have several labels. Mixed labels and repeated labels are errors. The switch value is read one time. `default` can be at any position. A string `switch` with a `break` inside an `if` and a `continue` for an outer loop is an error.

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

`enumerate` and `range` are global functions. A local, a member, a type or a native global of the same name hides them. In `enumerate`, `i` is a copy and `x` is a reference to the element. `var&` over `enumerate` is an error. The item type is `std::pair<size_t, T&>`. `range` gives values of the common type of its arguments.

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

A lambda, a `??` right side and a `finally` block can use a binding. A `var&` binding is a borrowed name, so a lambda cannot capture it.
