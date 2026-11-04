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

The string must use double quotes. A backtick string is an error. Write `\${` to get a literal `${`. A `{` or `}` in the text of an interpolated string prints as one brace: the compiler doubles it for you. A string with no `${` stays a plain literal. A script enum prints as its underlying integer. A native `enum class` has no formatter. It is an error at the `${` string: cast the value to its integer type, for example `${static_cast<int>(value)}`.

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

`a ?? b` works for a pointer, a handle and a `T?`. When `b` is known as not null, the result is known as not null too.

`?.` works on any expression: `a?.b`, `a?.F(x)`, `a?.b?.c`, `var n = a?.b;`, `return a?.b;` and an argument. The receiver `a` runs one time. The result type is the member type made nullable:

- A pointer or a handle member stays that pointer or handle. It is null when `a` is null.
- A value member (`int`, a struct, a string) gives `T?` (`std::optional<T>`). It is empty when `a` is null.
- A call of a function that returns `void` is a statement only: `h?.F();` does nothing when `h` is null.

At the start of an expression statement, `h?.F();` and `h?.x = 1;` become an `if`. Inside an expression, the translator writes `hot::safe_access(a, [&](auto&& __safe0) { return ...; })`. The code in the lambda cannot hold an `await`. A `?.` cannot be the target of an assignment inside an expression. A `.` after a `?.` reads the nullable result: `a?.b.c` is `(a?.b).c`, so write `a?.b?.c`.

```hot
class Pick {
    $Pick next = null;
    int x = 0;
    void F() { }

    $Pick Choose($Pick a, $Pick b, $Pick c) {
        var p = a ?? b ?? c;                  // a ?? (b ?? c)
        p?.F();
        p?.x = 1;
        var n = p?.x;                         // int?
        var q = p?.next?.next;                // a handle that can be null
        return p;
    }
}
```

## Checked unwrap

A cast to the same nullable type removes the null from the value. It throws `hot::NullReference` when the value is null. This test is always on. It does not depend on `HOT_RUNTIME_CHECKS`.

- `(T*)p`, where `p` is `T*`, writes `hot::nonnull(p)`. The result is not null.
- `($C)h`, where `h` is `$C` or a `$Derived` of a script class `C`, writes `hot::nonnull(h)`. An upcast also keeps the handle conversion.
- A downcast `($Derived)h` keeps `hot::as_script`. The result can be null.
- `(const T*)p` keeps the null state of `p`.
- `(T)x` for `x` of type `T?` is not a cast.

After a checked unwrap of a local name, the name is not null.

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
    $Unit seen = null;

    void Look() {
        if (seen == null) seen = new $Grunt();
        $Unit s = seen;
        if (s == null) return;
        bool isGrunt = s is $Grunt;
        var g = s as $Grunt;
        if (g != null) s.hp = g.rage;
        if (s is $Grunt h && h.rage > 1) s.hp = h.rage;
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
| `new $T()` | `new T()` | `.hot` gives a handle. `new T()` on a script class gives a value. `.cpp` gives a raw pointer, and the script code must `delete` the object. |
| `T` (a handle) | `T*` or `T&` | A `.cpp` script cannot hold a script object by value. |
| None | `typeid(T)` | An error in a `.cpp` script. Use `dynamic_cast`. |
