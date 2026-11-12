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

## Name a lambda

An attribute before a lambda gives it a name. The name is the key of a reloadable lambda.

```hot
var h = [name("onFire")] (CarveRequest r) => { };
var g = [name("onFire"), alias("onShoot")] (CarveRequest r) => { };
```

- `name("key")` sets the name. The argument is a string. A lambda has one `name`.
- `alias("old")` gives an old key of a renamed lambda. A lambda can have many `alias` attributes.
- Every attribute has an argument list. Another attribute name is an error.
- The attribute needs a lambda after it. The text `[name("x")] (a + b)` is an error.
- A list literal is still valid. The text `[name("x")]` alone is a list with one call.

The translator gives each lambda a key (`src/translator/lambda_key.h`). The generated C++ does not change.

- The key is `<unit>:<Class>::<function id>/<part>`. A free function has no `<Class>::`.
- The function id is the id of the method slot: the overload key, or the id of a native slot for an override. A constructor uses `ctor__ov_<signature>`.
- A nested lambda adds its own `/<part>` after the part of its parent.
- The part is `n:<name>` for a named lambda. Else it is `v:<variable>` when the lambda is the direct initializer of a local, or the direct right side of an assignment to a local or a field. Else it is `#<ordinal>`.
- The ordinal counts the lambdas of the function that have no name and no variable, in source order.
- Each `alias("old")` gives an alias key with the part `n:<old>`.
- Two equal keys in one function are an error. For a variable, add `[name("...")]`.
- A bound method group has the part `m:<object>.<Method>`. Two equal `m:` parts are not an error: they get an ordinal `#<n>`. See [Keys](#keys).
- An `alias` without `name` is an error.
- A lambda in a field initializer is an error, so it has no key.

## Lambdas and reload

Native code can keep a lambda, for example a timer. The reload commit gives a kept lambda the new body when its key, its signature and its captures agree. Else the kept lambda detaches: a call does nothing and logs a stale call. See [Rebind at the commit](lambda-hot-reload.html#rebind-at-the-commit).

```hot
using "Actor.h";

class Beeper : Actor {
    int fired = 0;

    // The timer holds this lambda. After a reload, the timer runs the new body.
    void arm(ref World w, float delay) {
        w.AddTimer(delay, () => { fired = fired + 1; });
    }
}
```

## Method groups

A method name without a call is a method group, as in C#. A method group is valid only as a call argument. It gives a handler to a native API.

```hot
using "Actor.h";
using "Signal.h";

class Ship : Actor {
    int hits = 0;

    void HandleHit(int n) { hits = hits + n; }

    void Arm(ref Signal<int> onHit, ref EventHub hub, $Ship other) {
        onHit.Subscribe(this, HandleHit);       // bound to this
        onHit.Subscribe(this, this.HandleHit);  // the same
        onHit.Subscribe(this, Ship.HandleHit);  // unbound: the signal gives the receiver
        hub.SubscribeToEvent(7, HandleHit);
        if (other != null) {
            onHit.Subscribe(this, other.HandleHit); // bound to the local "other"
        }
    }
}
```

### Name resolution

- `M` and `this.M` are a bound group when `M` is an instance method of the class. A local, a parameter, a field or a property with the name `M` hides the method.
- `o.M` is a bound group when `o` is a local or a parameter of a script class type, and `M` is a method of that class. A field or a property `M` of the class hides the method.
- `C.M` is an unbound group when `C` is a script class and `M` is an instance method of `C`.
- A static method and a free function are not method groups.
- The callee of a call is a call, not a group.

### Lowering

- A bound group is the lambda `(P... a) => o.M(a...)`. The parameter types `P` are the types of the method. The body calls the method with the normal call lowering.
- The lambda captures the object. `this` gives `__self`. A local gives a copy of its handle. The handle keeps the object alive.
- The lambda is a lambda box (`make_lambda_deduced`). The closure has the signature of the method, so a native template parameter takes it directly. A `std::function` parameter and a delegate take it too.
- A reload gives the box the new method body. A removed method or a changed signature detaches the box. The commit logs one warning for each key.
- An unbound group `C.M` is a `hot::MethodRef` (`include/hot/runtime/method_ref.h`). Its call operator takes `(C_Shell*, P...)`, so an API with a receiver and a handler can call it. The call goes through the slot of `M` (`hot::call`). The object holds only a pin of its generation.
- A call of an unbound group after a change of the signature detaches. After the removal of `M`, the call reports the script error of a removed function.
- Optimized AOT code has no reload. There an unbound group is a plain C++ lambda.

### Keys

The key of a bound group has the part `m:<object>.<Method>`: `m:this.HandleHit` for `HandleHit` and `this.HandleHit`, and `m:other.HandleHit` for the local `other`. Two equal parts in one function are legal: two subscriptions of one method are valid. The second equal part gets the suffix `#1`, the third `#2`, and so on, in source order. In a lambda, the key of the lambda is the prefix. An unbound group has no key: the slot gives its identity.

### Errors and limits

| Code | Error |
|---|---|
| `var f = HandleHit;`, `f = this.HandleHit;`, `return o.HandleHit;` | `'HandleHit' is a method group. A method group is accepted only as a call argument; call it, or write a lambda` |
| `Take(M)` where `M` has overloads | `ambiguous method group 'M': the method has overloads. Write a lambda` |
| `Take(this.target.M)`, `Take(target.M)` with a field `target` | `the method group 'target.M' needs 'this' or a local as its object. Copy the object into a local` |
| `Subscribe(this, Other.M)` in a method of another class | `the unbound method group 'Other.M' needs a method of the class '<Class>'` |

- An overloaded method is always ambiguous in this version. The target parameter type does not select the overload. Write a lambda.
- The object of a bound group is `this`, a local or a parameter. A field would give the value at the call time, not at the creation time.
- An unbound group needs a method of the class of the function, not of a script parent.
- `SubscribeToEvent(E, C.M)` does not compile. The engine form gives an `Object*` receiver, and a `MethodRef` takes the shell of `C`. Use the bound group `M`.
