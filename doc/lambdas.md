---
layout: default
title: Lambdas
nav_order: 9
---

A lambda is an arrow function with a block body or an expression body. It captures by value.

## Syntax

The form is `(params) => { body }` or `(params) => expression`. The expression form means `{ return expression; }`. A lambda can go to a `std.function` parameter or a native timer.

```spp
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

## Captures

A lambda copies the locals that it uses. It also reaches the members of `this`. A later change of the local does not change the copy.

```spp
using "Actor.h";

class Beeper : Actor {
    int fired = 0;

    void arm(World& w, float delay) {
        int step = 5;
        w.AddTimer(delay, () => {
            fired = fired + step;
            std.printf("fired=%d\n", fired);
        });
    }
}
```

## Lambdas and reload

Native code can keep a lambda, for example a timer. A reload does not change the body of a kept lambda. A call after the reload does nothing and logs a stale call. A lambda that the script creates after the reload uses the new body.

```spp
using "Actor.h";

class Beeper : Actor {
    int fired = 0;

    // The timer holds this lambda. After a reload, the old timer is stale.
    // A new call of arm() makes a timer with the new body.
    void arm(World& w, float delay) {
        w.AddTimer(delay, () => { fired = fired + 1; });
    }
}
```
