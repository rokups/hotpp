---
layout: default
title: Classes
nav_order: 8
group: Hot script
---

A class holds fields and methods. A bare name reaches a member of `this`. This works everywhere: in classes, in structs, before a `.` and for native members of the base class. Write `this.` only to pick the member when a parameter or a local has the same name.

The compiler resolves a bare name in this order:

1. A local or a parameter.
2. A script member: this class first, then the base classes. A `static` method of the class chain is a script member.
3. A native member of the base class. A module variable of the same name wins over it.
4. A free function, a module variable, a type, a native global or a namespace. A free function and a module variable cannot have the same name.
5. The helper functions `enumerate` and `range`.

## Parts of this guide

This guide has 2 pages. This page covers fields, methods, short bodies, constructors, destructors, properties and structs.

- [Mutual use, this and access words](classes-use.html): classes that use each other, `this` as a value, access words and renamed fields.

## Fields and methods

A field can have an initializer. A method that does not override a native method is a plain script method. An `abstract` method has no body (see [Abstract methods](inheritance.html#abstract-methods)). A class can also implement an interface (see [Interfaces](inheritance.html#interfaces)).

```hot
class Counter {
    int count = 0;
    std.string label;           // default value

    void Add(int n) { count += n; }
    int Get() { return count; }
    void Set(int count) { this.count = count; }   // this. tells the field from the parameter
}
```

Do not write `const` on a field or on a method. A field is never const. A method has no `const` after the parameters. The translator infers the constness of a local variable and of a parameter in the body. A `virtual` or `override` method takes its constness from the native base. The error is `const is inferred; remove it`.

## Short bodies and default values

`=>` gives a method or a read-only property an expression body. A parameter can have a default value. Put it on the first declaration. Constructors and destructors need a block body.

```hot
class Calc {
    int bonus = 2;

    int Double(int x) => x * 2;
    int Bonus => bonus * 10;                 // read-only property
    void Log(int x) => std.printf("%d\n", x);
    int Add(int a, int b = 1, int c = 0) { return a + b + c; }
}
```

A default value is an error on a `virtual` or `override` method. A struct constructor cannot have a default for every parameter. Calls from script code and the engine factory use the default value. It is not part of the reload key, so a change of a default value does not need a new key.

## Method overloads

A class can have two or more methods with one name and different parameter types. C++ selects the overload by the argument types of the call.

```hot
class Scorer {
    int Score(int x) => x * 2;
    int Score(float x) => 1000;
    virtual int Kind(int x) => 1;
    virtual int Kind(std.string s) => 2;
}

class Child : Scorer {
    override int Kind(int x) => 10;   // replaces Kind(int) only
}
```

- Each overload has its own slot. The key of the slot is the name and the parameter types, for example `Score__ov_int32_t`. A name with one method keeps the plain key, so its slot does not change.
- A reload can add an overload, remove one or change a parameter type. A live object keeps its fields.
- When a name gets a second overload, the first overload gets a new key. Its old slot becomes a removed function. Code of an older generation that calls the old slot gets the error of a removed function.
- An `override` replaces the parent overload with the same parameter types.
- The pass mode (`ref`, `in`) does not make two overloads. `$C` (a handle) and `C` (a value) are two types.
- A static method can have overloads. Its key is `<Class>::<name>__ov_<sig>` in the function table of the unit. An importer calls each overload.
- A free function can have overloads too (see [Free functions](module-level.html)).
- A struct method can have overloads. A struct method is a C++ member of the struct, so it has no key.
- A constructor can have overloads (see [Constructors and destructors](#constructors-and-destructors)).
- `base.Name(args)` calls the parent overload that the argument types select. The forwarder `<Parent>__<name>__ovbase` makes a direct call, also to a parent in an imported unit.
- An operator has no overloads.

| Mistake | Error |
|---|---|
| Two overloads with the same parameter types | `function '<name>' is declared twice with the same parameter types` |
| A child class adds an overload to a name of the parent | `function '<name>' adds an overload to the overloads of '<parent>'; declare all overloads of a name in one class` |
| More than one overload fits the call | `the call of '<Class>.<name>' fits more than one overload. Convert the argument to the parameter type of one overload` |

The errors are the same for a static method, a free function and a struct method. The call of a struct method has only the method name in the error.

## Constructors and destructors

The constructor has the class name. The destructor starts with `~`. `new` calls a constructor. `new $Helper()` gives a handle, and `new Helper()` gives a value. `Helper()` without `new` is an error.

```hot
class Counter {
    int count = 0;

    Counter(int start) { count = start; }
    ~Counter() { std.printf("gone %d\n", count); }
}

class User {
    $Counter c = null;

    void Make() {
        c = new $Counter(10);
    }
}
```

A reload does not run a constructor again. A changed constructor body applies to new objects only. If a constructor body throws, the object is deleted and the destructor still runs. A class has one destructor. Two overloads cannot differ only in the pass mode of a parameter.

The `__` prefix is reserved. Do not start the name of a field or a parameter with `__`.

## Properties

A property has accessors. `get; set;` makes an automatic property. In a custom setter, `value` is the new value.

```hot
using "Actor.h";

class Gauge : Actor {
    float Hp { get; set; } = 50.0;          // automatic
    float MaxHp { get; } = 100.0;           // read only
    int Level { get; private set; } = 1;    // private setter
    float raw = 0.0;
    float Shield {                          // custom accessors
        get { return raw; }
        set { raw = value < 0.0 ? 0.0 : value; }
    }
    float Armor {                           // expression bodies
        get => raw * 2.0;
        set => raw = value / 2.0;
    }
}
```

An accessor body can be an expression: `get => e;` is `get { return e; }`, and `set => e;` is `set { e; }`.

## Structs

A `struct` is a value type: a copy is a real copy. A bare `class` name is a value that owns its object, and `$C` is a handle. See [Value uses](types-handles.html#value-uses).

A struct with the base `ScriptError` is an exception type. See [Script exception types](statements-errors.html#script-exception-types).

```hot
struct Point {
    float x = 0.0;
    float y = 0.0;
}

struct Stats {
    float hp = 100.0;
    float Effective() => hp * 2.0;   // bare field in a struct
}

class Holder {
    Point p;
    Stats s = Stats();

    void Copy() {
        Point q = p;            // a copy
        q.x = 1.0;
    }
}
```

A script type cannot have the name of a C++ type from a `using` header. This applies to a class, a struct and an enum. The module would bind to the member functions of the host type and corrupt memory. The translator stops with this error:

```
the script type 'Item' has the same name as a C++ type from the header 'Object.h'. Rename the script type.
```

## Static methods

A `static` method belongs to the class and has no object. Write `static` before the return type. A struct can have static methods too.

```hot
class Hero : Actor {
    int hp = 0;

    static int Clamp(int v, int top) { return v > top ? top : v; }

    override void Tick(float dt) {
        hp = Clamp(hp + 1, 100);       // bare name inside the class
    }
}

class Spawner : Actor {
    override void Tick(float dt) {
        int h = Hero.Clamp(150, 100);  // class name outside the class
    }
}
```

The body has no `this` and cannot use a field of the class. It can call other static methods and free functions. It can read and write a module variable and a static field. The word `static` combines with access words (`public`, `private`) only.

Call forms:

- `C.F(x)` works everywhere.
- A bare `F(x)` works inside the class `C` and inside its script subclasses.
- `D.F(x)` works when `D` is a script subclass of `C` and `C` has the static method `F`.
- `C.F(x)` works in another module that imports `C`. The call goes through `hot::call_fn` to the owner module.
- A lambda in the class can call `F(x)` and `C.F(x)`.
- `obj.F(x)` is an error.

The translator writes a static method as a plain C++ `static` function. The name is `<Class>__s_<name>`. For `Clamp` in `Hero`, the name is `Hero__s_Clamp`. The function has no slot, no thunk and no manifest entry. The host cannot call it through `Loader::thunk`. Put the call in an instance method if the host must start it.

A class or a struct can have static fields and static properties. See [Static fields](#static-fields) and [Static properties](#static-properties).

### Class constants

`static const T Name = value;` in a class or a struct is a class constant. The types are the types of a [module constant](module-level.html#module-constants): a number, `bool`, an enum, `const char*`, a native class, or a native constexpr call.

```hot
class Weapon : Actor {
    public static const float MaxRange = 50.0f;
    private static const int Ammo = 30;
    int Pick(int v = Ammo) { switch (v) { case Ammo: return 1; default: return 0; } }
}
float F() => Weapon.MaxRange;
```

- Write `C.Name` anywhere. In `C` and in a script child of `C`, the bare `Name` also works.
- A parameter or a local with the same name hides the constant in its function. `C.Name` still works there.
- A `case` label, a default value and another constant can use a class constant.
- `private` keeps the constant in `C`. `protected` keeps it in `C` and its script children.
- An importer sees a public or protected constant. A new value changes the interface hash.
- The translator writes `static constexpr` at file scope with the name `<Class>__<Name>`. A type that is not a literal type (a string class, a vector) gets `static const`.
- A reload gives the new value, as for a module constant.
- `const T Name` without `static` in a class is an error.

| Code | Error |
|---|---|
| `obj.F(x)` | `'F' is a static method; call it as 'C.F(...)'` |
| `var f = C.F;` or `var f = F;` | `'F' is a function; call it, or write a lambda` |
| `static async void F()` | `an async function needs an object; remove 'static'` |
| `static virtual` or `static override` | `'static' and 'virtual' cannot combine` (the same for `override`) |
| A `static` constructor | `a constructor cannot be 'static'` |
| A `static` member of an interface | `an interface member cannot have the modifier 'static'` |
| Two static methods with the same parameter types | `function '<name>' is declared twice with the same parameter types` |
| A static method and a method with one name | `function '<name>' is declared twice (a static method and a method)` |
| A call of a function in a field initializer | `a field initializer cannot call the function '<name>': the prototype of the function comes after the class` |
| A call of a function in a default argument | `a default argument cannot call the function '<name>': the prototype of the function comes after the declaration` |

A static method can have overloads (see [Method overloads](#method-overloads)).

### Static fields

`static T Name;` or `static T Name = value;` in a class or a struct is a static field (gap G23). It has one instance for the program.

```hot
class Spawner : Actor {
    static int count_ = 0;
    private static float scale_ = 1.0f;
    override void Tick(float dt) { count_++; Spawner.count_ += 2; }
    static int Count() => count_;
}
int F() => Spawner.count_;
```

- A static field is a module variable of the owner module with the name `<Class>__<Name>`. The module block holds it.
- The types, the initializer rules and the errors are the ones of a [module variable](module-level.html#module-variables).
- Write `C.Name` anywhere. In `C` and in a script child of `C`, the bare `Name` also works. A lambda and a static method can use it.
- A parameter or a local with the same name hides the field in its function.
- `private` keeps the field in `C`. `protected` keeps it in `C` and its script children.
- An importer reads and writes a public or protected static field through `hot::mod_var`. The owner module keeps the storage.
- The interface hash changes when a public or protected static field is added, removed or retyped. A private one is not in the interface.
- Native code has no access to a static field.

### Static properties

A static property has the syntax of a [property](#properties) with `static` before the type.

```hot
class Spawner : Actor {
    static int count_ = 0;
    static int Count { get { return count_; } set { count_ = value; } }
    static int Made { get; private set; } = 0;    // automatic
    static int Twice => count_ * 2;               // read only
}
void F() { Spawner.Count = 5; Spawner.Count += 1; int t = Spawner.Twice; }
```

- The translator makes the static methods `get_<Name>` and `set_<Name>`. `get; set;` adds a hidden static field `<Class>____p_<Name>`.
- A read is a call of `get_<Name>`. A write statement is a call of `set_<Name>`: `C.P = e`, `C.P op= e`, `C.P++` and `C.P--`.
- Another write is an error, for example `int a = (C.P = 1);`. The error is `a write of the static property 'C.P' must be a statement`.
- A write without a `set` gives `'C.P' has no 'set'`. A read without a `get` gives `'C.P' has no 'get'`.
- The access of each accessor is the access of its static method. An importer calls the accessors through `hot::call_fn`.
- The interface hash changes when a static property is added, removed or retyped.

### Static methods and reload

A static method has no slot. A reload replaces the code of the whole module. The next call of `C.F(x)` runs the new body. A change of the signature is a normal code change, because a caller in the same module changes with it. A static method of an imported class follows the class (see [Imports](module-level.html#imports)).

A reload keeps the value of a static field when its name and its type stay. A new type resets the field to its initializer. A static property follows its accessors and its field.

## Operators

A class or a struct can define operators. An operator is a member method. Its left operand is `this`. The right operand is the parameter. There is no operator outside a type.

```hot
struct Vec2 {
    float x = 0.0;
    float y = 0.0;
    float[2] data;

    Vec2(float ax, float ay) {
        x = ax;
        y = ay;
    }

    Vec2 operator+(in Vec2 o) { return Vec2(x + o.x, y + o.y); }
    Vec2 operator-() { return Vec2(-x, -y); }
    Vec2 operator*(float f) { return Vec2(x * f, y * f); }
    bool operator<(in Vec2 o) { return x < o.x; }
    bool operator>(in Vec2 o) { return x > o.x; }
    ref float operator[](int i) { return data[i]; }
}

class Holder {
    void Use() {
        Vec2 a = Vec2(1.0, 2.0);
        Vec2 b = Vec2(3.0, 5.0);
        Vec2 c = a + b;
        c += b;             // generated from operator+
        c *= 2.0;           // generated from operator*
        Vec2 d = -a;
        a[0] = 4.0;         // the ref result is an lvalue
        bool less = a < b;
        bool same = a == b; // generated field-wise ==
        bool diff = a != b; // generated from ==
    }
}
```

These operators are allowed:

- Binary: `+ - * / % & | ^ << >>`.
- Compare: `== < > <= >=`. Each one returns `bool`.
- Index: `[]`. It takes one parameter and does not return `void`.
- Unary: `+ - ! ~`. A unary operator takes no parameter. `+` and `-` can have both forms. They are two different operators.

Rules:

- A type has one operator for each symbol. There are no overloads.
- `!=` comes from `==`. The compound forms (`+=`, `-=` and the others) come from the binary operators. A compound operator exists when the binary operator returns the own type.
- The ordering uses single operators. `<` needs `>`, and `<=` needs `>=`. There is no `<=>`.
- A struct has a generated field-wise `==` and `!=`. A written `operator==` replaces it.
- A bare class value (a box) has no `==` unless the class writes `operator==`. See [Value uses](types-handles.html#value-uses).
- An operator does not apply to a handle (`$C`) or to an optional (`C?`). The only exception is `==` and `!=`, which compare the identity of two handles.
- The right operand of an operator on a class value must be a value. `a + null`, `a + h` and `a < h` (`h` is a handle) are errors. `a == null` and `a == h` keep the error of the box compare. A value of an unrelated class is also an error.
- A compound operator (`+=`) exists only in the class that declares the binary operator, and only when the operator returns that class. `d += x` is an error when `d` is a subclass and `operator+` comes from the parent. `a += b` is an error when the type has no `operator+`, or when `operator+` returns another type. The plain `d + x` works on a subclass. Its result has the type that `operator+` returns.
- `a[i] = v`, `a[i] += v` and `a[i]++` need `ref T operator[]`. An `operator[]` that returns a value gives a temporary, so the write is an error.
- A comparison, and any operator with a value return, cannot change the object. A write to a field in its body (`x = 1`, `x += 1`, `x++`, `this.x = 1`, `arr[i] = 1` on a stored array or container) is an error. A call of a function of the type that writes a field is also an error. Only an operator that returns `ref T` can write. The generated member of the operator is `const`.
- A function cannot use the name of an operator identifier (`op_add`, `op_index` and the others). Write `operator+`.

The translator reports these errors. The text in quotes is the message.

| Source | Message |
| --- | --- |
| `bool operator!=(in V o)` | `operator '!=' is generated from 'operator=='; remove it` |
| `V operator+=(in V o)` | `operator '+=' is generated from 'operator+'; remove it` |
| `operator<` with no `operator>` | `operator '<' needs 'operator>'` |
| `int operator==(in V o)` | `operator '==' must return bool` |
| `void operator[](in int i)` | `operator '[]' cannot return void` |
| `V operator*()` | `operator '*' takes one parameter` |
| `bool operator!(in V o)` | `operator '!' takes no parameter` |
| Two `operator+` in one type | `operator '+' is declared twice (overloads are not supported)` |
| `a.op_add(b)` | `'op_add' is an operator; write 'a + b'` |
| `h + h` where `h` is a handle | `operator '+' needs a value, not a handle` |
| An operator in an `interface` | `an operator needs a class or a struct` |
| `void operator+(in V o)` | `operator '+' cannot return void` |
| `V op_add(in V o)` | `'op_add' is reserved for an operator; write 'operator+'` |
| `x = 1` in `operator+` that returns a value | `operator '+' cannot change the object; return a new value instead` |
| `d += x` with `operator+` from the parent | `operator '+=' is not available on 'D': 'operator+' comes from the parent 'B'. Declare 'operator+' in 'D'` |
| `a += b` with no `operator+`, or a `+` that returns another type | `operator '+=' needs 'operator+' in 'T'`, `operator '-=' needs 'operator-' of 'T' to return 'T'` |
| `a + null`, `a + h` | `operator '+' needs a value, not null`, `operator '+' needs a value, not a handle` |
| `v + w` where `v` is `T?` | `operator '+' needs a value, not an optional` |
| `a[0] = v` with `V operator[](...)` | `operator '[]' must return 'ref T' to be assigned` |

### Operators and reload

An operator is a method. Its slot key is, for example, `Money::op_add`. A reload changes the body of an operator as it changes the body of any method. The usual rules for methods apply: see [Hot reload](hot-reload.html).

### Operators on a class

A class value (a box) uses the operators of its class. `==` on two boxes needs `operator==`. A handle comparison with `==` compares identity.

```hot
class Money {
    int cents = 0;

    Money(int c) { cents = c; }

    Money operator+(in Money o) { return new Money(cents + o.cents); }
    bool operator==(in Money o) { return cents == o.cents; }
}

class Wallet {
    Money a = new Money(100);
    Money b = new Money(100);
    $Money h = new $Money(100);
    $Money k = new $Money(100);

    void Use() {
        Money c = a + b;
        bool same = a == b;     // calls operator==
        bool ident = h == k;    // identity of two handles
    }
}
```

An `in Money` parameter and a `Money` result work on all methods of a class value.

### Operators and C++

The generated C++ struct has real member operators. The host can use them. For example, `std::sort` works on a `std::vector` of a struct that has `operator<`.

The limits of operators are in [Limits](limits.html).
