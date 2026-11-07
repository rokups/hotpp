---
layout: default
title: Lambdas
nav_order: 15
group: Hot script
---

A lambda is an arrow function with a block body or an expression body. It captures by value.

## Syntax

The form is `(params) => { body }` or `(params) => expression`. The expression form means `{ return expression; }`. A lambda can go to a `std.function` parameter or a native timer.

```hot
using "Actor.h";

class Calc : Actor {
    int total = 0;

    int Apply(std.function<int(int)> fn, int v) {
        return fn(v);
    }

    void Run() {
        int k = 3;
        std.function<int(int)> twice = (int x) => { return x * 2 + k; };
        total = Apply(twice, 4);
        total = Apply((int x) => x + 1, 4);          // expression body
    }
}
```

A `std.function` value tests against `null`. `f == null` gives `!f` and `f != null` gives `static_cast<bool>(f)`. `f = null` clears it.

## Captures

A lambda copies the locals that it uses. It also reaches the members of `this`. A later change of the local does not change the copy. A lambda that writes a local makes the local a shared cell (see below).

```hot
using "Actor.h";

class Beeper : Actor {
    int fired = 0;

    void arm(ref World w, float delay) {
        int step = 5;
        w.AddTimer(delay, () => {
            fired = fired + step;
            std.printf("fired=%d\n", fired);
        });
    }
}
```

### A lambda that writes a captured variable

A lambda can write a local or a plain parameter of its function. The variable is then a shared cell, as in C#. No keyword is necessary. The variable lives on the heap. The function and each lambda use the same cell, so all of them see each write. The cell lives while a lambda or the function holds it.

```hot
using "Actor.h";

class Counter : Actor {
    int counted = 0;

    void count() {
        int n = 0;
        var inc = () => { n++; };
        inc();
        inc();
        counted = n; // 2
    }
}
```

The translator writes `auto __cell_n = hot::make_cell<int32_t>(0); int32_t& n = *__cell_n;` (`include/hot/runtime/cell.h`). The lambda copies `__cell_n` and binds `n` to it again. A parameter gets a cell with its value at the start of the body. A write is an assignment, `++`, `--`, a `ref` or `out` argument, or a call of a member function on a value. A lambda that only reads a cell also uses the cell.

Only a local or a plain parameter can be a cell. A lambda that assigns a `foreach` variable, a structured binding, a `catch` variable, a parameter of an outer lambda or a local in the init part of a `for` is an error. Copy the value into a local before the lambda. A `ref`, `in` or `out` parameter cannot go into a lambda (see [Captures](#captures)).

## Lambdas and reload

Native code can keep a lambda, for example a timer. A reload does not change the body of a kept lambda. A call after the reload does nothing and logs a stale call. A lambda that the script creates after the reload uses the new body.

```hot
using "Actor.h";

class Beeper : Actor {
    int fired = 0;

    // The timer holds this lambda. After a reload, the old timer is stale.
    // A new call of arm() makes a timer with the new body.
    void arm(ref World w, float delay) {
        w.AddTimer(delay, () => { fired = fired + 1; });
    }
}
```
