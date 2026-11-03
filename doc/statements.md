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

## Reserved words

The words `extern`, `ref` and `out` are reserved. `extern` is a reserved word only: it has no syntax. Native declarations are `[native]` stubs and header imports. A variable, field, parameter, method or class cannot have one of these names. Names such as `refx`, `outer` and `external` are valid.

## Const is inferred

Do not write `const` on a local variable. The translator makes a local const when the body never assigns it after the declaration. The error is `const is inferred; remove it`. These uses count as a write: an assignment, `++` or `--`, `&x`, `x[i]` on a value, a call of a method on `x`, and a pass of `x` to a function that takes it by reference. A `ref` local is never const. A local with no initializer is not const. A cast can still name `const`: `(const T*)p`.

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

## `switch`

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

## `foreach`

`foreach` walks a container. `var x` and `T x` copy each element. `ref x` binds a reference to the element. A `ref x` that the body never writes is a const reference.

```hot
using <map>;

class Sum {
    int Total(ref std.vector<int> xs) {
        int sum = 0;
        foreach (var x in xs) sum += x;
        foreach (int y in xs) sum += y;
        foreach (ref z in xs) sum += z;           // no copy
        return sum;
    }

    void Double(ref std.vector<int> xs) {
        foreach (ref x in xs) x = x * 2;          // changes the element
    }

    int Pairs(ref std.map<std.string, int> m, ref std.vector<int> xs) {
        int sum = 0;
        foreach (var (k, v) in m) sum += v;                        // key and value
        foreach (var (i, x) in enumerate(xs)) sum += i * x;   // index and element
        foreach (int i in range(3)) sum += i;                  // 0, 1, 2
        foreach (int i in range(2, 5)) sum += i;               // 2, 3, 4
        return sum;
    }
}
```

`enumerate` and `range` are global functions. A local, a member, a type or a native global of the same name hides them. In `enumerate`, `i` is a copy and `x` is a reference to the element. `ref` over `enumerate` is an error. The item type is `std::pair<size_t, T&>`. `range` gives values of the common type of its arguments.

## Structured bindings

`var (a, b) = e;` names the parts of a `std.pair`, a `std.tuple`, a struct or a map entry. It needs two names or more. `ref` binds references.

```hot
class Split {
    std.pair<int, float> pair = std.make_pair(3, 4.5);

    std.pair<int, float> Make() => std.make_pair(1, 2.5);

    void Run() {
        var (a, b) = Make();       // copies
        ref (c, d) = pair;        // references into the field
        c = a + 1;
    }
}
```

A lambda, a `??` right side and a `finally` block can use a binding. A `ref` binding is a borrowed name, so a lambda cannot capture it.
