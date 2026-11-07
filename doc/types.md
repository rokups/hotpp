---
layout: default
title: Types
nav_order: 4
group: Hot script
---

Hot++ has fixed-width builtin types. Other types come from C++.

## Parts of this guide

This guide has 2 pages. This page covers builtin types, variables, C++ types, collection literals and arrays.

- [Handles, references and pointers](types-handles.html): handles, weak handles, values and handles of script classes, references and pointers.

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

## Integer arithmetic and casts

Integer operations have no undefined behavior in a script, except where this section says so.

- Signed overflow wraps. `INT_MAX + 1` is `INT_MIN`, and `-INT_MIN` is `INT_MIN`. The script compile uses `-fwrapv` for the JIT backend, the Clang files backend and the AOT library.
- A divide or modulo by zero throws `ArithmeticError`. `INT_MIN / -1` wraps.
- `<<` and `>>` (and `<<=` and `>>=`) throw `ArithmeticError("shift count out of range")` when the count is negative or not less than the bit width of the promoted left operand. A `byte` or `short` operand promotes to `int`, so its width is 32.
- A left shift keeps the low bits. A shift of a negative value, or into the sign bit, wraps: `1 << 31` is `INT_MIN`.
- A right shift of a negative value is an arithmetic shift: `-8 >> 1` is `-4`.
- A cast from `float` or `double` to an integer type cuts the fraction. A value out of range gives the nearest limit of the type. `NaN` gives `0`. This is true for `(int)f`, `int(f)` and `static_cast<int>(f)`.

The shift check and the divide check are always on, also with `HOT_RUNTIME_CHECKS=0`. The translator skips a check when it can see that the check is not needed: a shift by a literal count in range, or a float operand. A class with its own `<<` or `>>` operator (a stream, for example) keeps the operator.

MSVC has no `-fwrapv`. The script compile in an MSVC build does not guard signed overflow. MSVC does not use signed overflow for optimization in practice, so a wrap is the result there too.

The implicit conversion from a float to an integer is not checked. Use a cast (see [Limits](limits.html)).

## Variables

A variable can have no initializer. It then gets the default value (zero, `false`, `null`). `var` takes the type from the initializer. Do not write `const` on a variable. The translator infers it: a local that the body never assigns is const. The error `const is inferred; remove it` shows when you write it.
 A field can use `var` too, for example `protected var logger = Urho3D.Log.GetLogger();`. The field type is the type of the initializer without a reference.

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
        int limit = 10;        // never assigned: the translator makes it const
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

## Type aliases

`using Name = type;` names a type (see [Module level](module-level.html#type-aliases)). An alias is the type that it names. `using Soldier = $Recruit;` is a handle. `using Soldier = Recruit;` is a value.

```hot
class Recruit {
    int hp = 3;
}

using Squad = std.vector<$Recruit>;   // a vector of handles
using Soldier = $Recruit;

class Platoon {
    Squad squad;
    Soldier lead;
}
```

## Script classes in templates

A template argument `$Child` is a handle. `std.vector<$Child>` holds one handle for each element. The rule also works inside other templates, for example `std.map<int, std.vector<$Child>>`.

```hot
using <vector>;

class Child {
    int hp = 3;
}

class Parent {
    std.vector<$Child> kids = std.vector<$Child>();

    void Run() {
        this.kids.push_back(new $Child());
        foreach (var k in this.kids) { k.hp += 1; }
        this.kids[0].hp = 9;
    }
}
```

- `kids[i]`, `front()`, `back()`, `at(i)` and the `foreach` variable give the handle. A member access on them checks for `null`.
- An index that is out of range is an error at run time, as for a native vector.
- A reload keeps the handles. A new field of `Child` does not reset the vector.
- A struct in a template stays a value.
- A bare script class in a template argument is a value: `std.vector<Child>` holds `hot::Value<Child>`. Write `$Child` for a handle.
- `std.shared_ptr<Child>` already is a handle type. The translator does not change it.
- A `.cpp` script keeps its own C++ types.

A native function template can get a script class as an explicit argument (G09). Examples are `node.GetComponent<PlayerStance>()`, `node.CreateComponent<PlayerStance>()` and `MakeShared<PlayerStance>()`.

- `C` and `$C` are the same here. The native template gets the plain class `C`.
- The result becomes a handle: `$PlayerStance s = node.GetComponent<PlayerStance>();`.
- A class that is not the target or a script child of it is an error. The error is `'GetComponent<User>' gives a 'User', and 'User' is not 'PlayerStance' or a script child of it`.

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

`T[N]` is an array of `N` values of type `T`. The size `N` is an integer literal. The C++ type is `std::array<T, N>` by default. The module attribute `array` sets another type (see [The array type](native-interop-attributes.html#the-array-type)). `T[M][N]` is an array of `M` arrays of `N` values.

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

- The size is a literal. `int[n]` is an error.
- The size is from 1 to 16777216 (`1 << 24`). `int[0]` and a larger size are errors.
- `is`, `as` and casts do not take an array type. `x is int[3]`, `x as int[3]` and `(int[3])x` are errors.
- An index is checked. An index that is negative or not less than the size throws `IndexOutOfRange` (see [Runtime errors](statements-errors.html#runtime-errors)). The build option `HOT_RUNTIME_CHECKS=OFF` removes the check. Then an index outside the array is undefined behavior. A custom array type from the module attribute `array` is checked also (see [The array type](native-interop-attributes.html#the-array-type)).
- `.size()` gives the number of values.
- A copy of an array copies all values.
- A reload keeps the values of an array field when the type stays the same.
- A new size is a new type. For example, `int[3]` becomes `int[4]`. Then the field gets its default value.

### Views

`T[]` is a view of values of type `T`. The C++ type is `hot::span<T>` by default. It works with C++17. A view does not own the values. See [The view type](#the-view-type) for the option that selects `std::span<T>`.

A view is valid for a parameter, a return type and a local. The owner of the values is a `std.vector<T>`, a `T[N]` or another view. A `std.vector<T>` or a `T[N]` converts to a `T[]` at a call or at a local declaration.

```hot
using <vector>;

class Stats {
    int Sum(int[] values) {
        int sum = 0;
        foreach (var x in values) {
            sum += x;
        }
        return sum;
    }

    void Double(int[] values) {
        foreach (ref x in values) {
            x *= 2;
        }
    }

    int Run() {
        std.vector<int> list = [1, 2, 3];
        int[3] fixed;
        int[] head = list;
        Double(head);
        Double(fixed);
        return Sum(list) + Sum(fixed) + head[0];
    }
}
```

These rules apply to a view:

- A view is an error on a field and on a property. The text is `T[] is a view; a field needs std.vector<T> or T[N]`. Use `std.vector<T>` for a list that the object owns. Use `T[N]` for a fixed array.
- `foreach (var x in s)`, `foreach (ref x in s)`, `s.size()` and `s.empty()` work on a view.
- An index is checked as for an array. An index that is negative or not less than `s.size()` throws `IndexOutOfRange`. The build option `HOT_RUNTIME_CHECKS=OFF` removes the check.
- A write through a view changes the values of the owner. A local that is the source of a `T[]` call or local is not `const`.
- A `T[]` does not accept a const source, for example an `in std.vector<T>` parameter. Take the same `in std.vector<T>` type for the parameter.
- A view is valid while the owner lives. A method that returns `T[]` or `std.string_view` cannot return a local owner (`std.vector<T>`, `T[N]` or `std.string`), a view local over such an owner, or a temporary vector. This is an error: the local ends at the return. Return a `std.vector<T>` or a `std.string`, or a view that the method got as an argument.
- A view does not stay valid after a change of the owner size. For example, `push_back` on the vector can move the values. The checker treats a view local (`T[] s = v;`) like a `ref` to an element of `v`. After a shape change of `v` (see "Lifetime of a ref" in [Type design](type-design.html#lifetime-of-a-ref)), a later use of `s` is an error. Assign the view again (`s = v;`) to use it. See [Limits](limits.html) for what the check does not see.
- A `std.string_view` or a `T[]` is an error as a field type, and as an element of a container field (`std.vector<std.string_view>`). A field of type `std.string_view` with a string literal as initializer is valid.

#### The view type

`hot::span<T>` is the default C++ type of `T[]`. It is in `hot/runtime/span.h`. It has these properties:

- It has a dynamic extent only. It has no fixed size in the type.
- It needs C++17. The build does not need C++20.
- It holds a pointer and a size. It does not own the values.
- Its own `operator[]` has no check, as for `std::span`. The index in a script goes through `hot::index`, which throws `IndexOutOfRange` for a bad index. `HOT_RUNTIME_CHECKS=OFF` removes the check.

A view converts to a native parameter of type `eastl::span<T>`, `std::span<T>` or `span<const T>`. A `const&` parameter of these types is also valid. The translator writes `hot::span_as(v)`. It gives a span of the same pointer and size. All overloads of the native function must take a span at that position.

You can select `std::span<T>` instead. Use the CMake option `HOT_VIEW_TYPE=std`. These rules apply:

- `std::span` needs C++20. With a lower `CMAKE_CXX_STANDARD`, the configure step stops with an error.
- The choice is one constant for the whole build. Do not mix the two types in one build. The two types have different C++ names, so a mix breaks the link of a host function with a `T[]` parameter and can break the One Definition Rule.
- The choice must be the same in `hotc` and in the host. `hotc` has the flag `--view=hot|std`. `hot_add_scripts` sets it. In build mode, a `--view` value that differs from the `hotc` build is an error. `hotc` prints `hotc: error: this hotc is built for --view=..., not --view=...` and exits with code 2. `--view=std` with `--std=c++17` is a translation error.
- Host code that uses the `T[]` type of a script method sees the selected type. Host code must use the same option as the scripts.

See [CMake options](cmake-options.html#options) for the option `HOT_VIEW_TYPE`.
