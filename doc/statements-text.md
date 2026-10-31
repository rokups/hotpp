---
layout: default
title: Strings, casts and patterns
nav_order: 14
group: Hot script
---

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

The string must use double quotes. A backtick string is an error. Write `\${` to get a literal `${`. A `{` or `}` in the text of an interpolated string prints as one brace: the compiler doubles it for you. A string with no `${` stays a plain literal.

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

`a ?? b` gives `a` when `a` is not null (or not false). Otherwise it gives `b`. The right side runs only when it is needed. `??` binds between `||` and `?:`. It is right associative. The left side is read one time.

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

## `is` and `as`

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

A `.cpp` script has the same rules with the C++ forms (see [C++ scripts](cpp-types.html#script-classes-as-types)):

| `.hot` | `.cpp` | Difference |
|---|---|---|
| `x is T` | `dynamic_cast<T*>(x) != nullptr` | None. |
| `x as T` | `dynamic_cast<T*>(x)` | `as` gives a handle. `dynamic_cast` gives a raw pointer. |
| None | `dynamic_cast<T&>(x)` | It throws `std::runtime_error` when the type does not match. |
| `new T()` | `new T()` | `.hot` gives a handle. `.cpp` gives a raw pointer, and the script code must `delete` the object. |
| `T` (a handle) | `T*` or `T&` | A `.cpp` script cannot hold a script object by value. |
| None | `typeid(T)` | An error in a `.cpp` script. Use `dynamic_cast`. |
