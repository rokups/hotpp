---
layout: default
title: Types
nav_order: 4
group: Hot script
---

Hot++ has fixed-width builtin types. Other types come from C++.

## Builtin types

| Type | Alias | Note |
|---|---|---|
| `bool` | | |
| `sbyte` | `int8` | |
| `byte` | `uint8` | |
| `short` | `int16` | |
| `ushort` | `uint16` | |
| `int` | `int32` | `5` |
| `uint` | `uint32` | |
| `long` | `int64` | `5L` |
| `ulong` | `uint64` | |
| `nint`, `nuint` | | pointer size |
| `float` | | `5.0` |
| `double` | | `5.0d` |
| `char`, `wchar` | | 8 bit and 16 bit |
| `void` | | no value |

`null` is the empty handle.

The C++ spellings with many words are errors, for example `unsigned int` and `long long`. The message gives the script type: use `uint` or `long`. A native virtual method with an `unsigned` parameter matches a script override with `uint`.

## Variables

A variable can have no initializer. It then gets the default value (zero, `false`, `null`). `var` takes the type from the initializer. `const` makes the variable read only.

```hot
class Demo {
    int hp = 100;
    ulong id = 7;
    float speed = 5.0;         // 5.0 is float, 5.0d is double
    double ratio = 0.5d;
    long big = 5L;
    int count;                 // default value: 0

    void Run() {
        var x = 3;             // int
        const int limit = 10;  // cannot change
    }
}
```

## C++ types

Use `.` for a scope. The translator writes `::` and `->` for you.

```hot
class Names {
    std.string name = "demo";
    std.vector<int> list = std.vector<int>();
}
```

`string`, `vector` and the other headers in [Module level](module-level.html) need no `using`.

## Collection literals

`[a, b]` builds a list. `[k: v]` builds a map. `[]` is empty. The C++ type comes from the declaration, so a literal needs a typed local, a typed field initializer or a place inside another literal. `var x = [1, 2];` is an error.

```hot
using <map>;

class Tables {
    std.vector<int> ids = [1, 2, 3];
    std.map<std.string, int> score = ["a": 1, "b": 2];

    void Run() {
        std.vector<std.vector<int>> grid = [[1, 2], [3]];
        std.vector<int> none = [];
    }
}
```

## Arrays

`T[N]` is an array of `N` values of type `T`. The size `N` is an integer literal. The C++ type is `std::array<T, N>`. `T[M][N]` is an array of `M` arrays of `N` values.

```hot
class Grid {
    int[4] cells;
    float[3] v = [1.0, 2.0, 3.0];
    int[2][3] table;

    int Sum() {
        int sum = 0;
        foreach (var x in cells) {
            sum += x;
        }
        return sum + cells.size();
    }
}
```

These rules apply to an array:

- The size is a literal. `int[]` and `int[n]` are errors.
- The size is from 1 to 16777216 (`1 << 24`). `int[0]` and a larger size are errors.
- `is`, `as` and casts do not take an array type. `x is int[3]`, `x as int[3]` and `(int[3])x` are errors.
- An index is checked. An index that is negative or not less than the size throws `IndexOutOfRange` (see [Statements](statements.html)). The build option `HOT_RUNTIME_CHECKS=OFF` removes the check. Then an index outside the array is undefined behavior.
- `.size()` gives the number of values.
- A copy of an array copies all values.
- A reload keeps the values of an array field when the type stays the same.
- A new size is a new type. For example, `int[3]` becomes `int[4]`. Then the field gets its default value.

## Handles

A script class is a handle. `new` makes the object and `null` clears the handle. A native class needs a stub to be a handle.

```hot
using "Actor.h";
using "Weapon.h";

[native, handle("std::shared_ptr<$T>")] class Weapon;

class Knight : Actor {
    Weapon weapon = null;

    override void Tick(float dt) {
        if (weapon == null) weapon = new Weapon(7, "sword");
    }
}
```

## References and pointers

A parameter `T&` is a reference. The call has no marker: write `f(x)`. A reference is borrowed: use it in the method, but do not store it in a field.

`&x` is the address of `x` and has the type `T*`: write `f(&x)`. `*e` reads through an iterator, an `std.optional`, a smart pointer or a handle. `&h` on a handle is an error: [Native interop](native-interop.html) shows the right forms.

`var& r = x;` and `const var& r = x;` make a reference local. `var r = x;` copies.

```hot
using "Actor.h";

class Bullet {
    int dmg = 1;
}

class Mover : Actor {
    Bullet shot = null;
    int n = 0;

    void Split(float total, float& half, int& calls) {
        half = total / 2.0;
        calls++;
    }

    void Make(Bullet& b) { b = new Bullet(); }            // b is the handle itself

    override void Tick(float dt) {
        float h = 0.0;
        int calls = 0;
        Split(dt, h, calls);                              // no marker
        Make(shot);

        var& r = n;                                       // a reference, not a copy
        r = 2;
        var copy = n;
        std.vector<int> xs = std.vector<int>();
        foreach (var& x in xs) x = x + copy;
        foreach (const var& x in xs) calls += x;
    }
}
```

A `T&` of a script class is a reference to the handle, so the callee can rebind it. A single `T*` of a script class is an error: write `T`. A native handle type keeps the raw `T&` and `T*`.

[Borrowing](borrowing.html) lists every place where a reference or a pointer is allowed.

Not supported: generics in script code, `::`, `->` and brace initializers. `?.` works only at the start of a statement: see [Statements](statements.html). Write `T(args)` instead of `T{args}`.
