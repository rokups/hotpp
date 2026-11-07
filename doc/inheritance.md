---
layout: default
title: Inheritance
nav_order: 10
group: Hot script
---

A script class can derive from native classes and from other script classes.

## Native base

A method replaces a native virtual method only with `override`. Without it the translator reports an error.

```hot
using "Actor.h";

class Player : Actor {
    override void Tick(float dt) {
        Move(Vec3(dt, 0, 0));
    }
    override void TakeDamage(float amount) { }
}
```

## Several native bases

The first base is the main base. The other bases are interfaces. Implement each pure virtual method with `override`. `base.` calls the interface code.

```hot
using "Actor.h";
using "Damageable.h";

class Boss : Actor, IDamageable {
    int hp = 50;

    override int Absorb(int amount) {
        hp = hp - amount;
        return hp;
    }
    int BaseArmor() { return base.Armor(); }
}
```

A script class can override a protected native virtual method: `protected override void OnX() { base.OnX(); }`. The shell calls the native code for `base.OnX()`.

These rules apply to the bases:

- The first base gets the constructor arguments.
- Each other base needs a default constructor.
- A signature in two bases is an error.
- A method pair `F()` and `F() const` takes the non-const method. See below.
- Only the root class of a family can have interface bases. A child class gets them from the root.

### Const methods

Two native bases can have `F()` and `F() const` as virtual methods. A script method never has `const` after the parameters. The error is `const is inferred; remove it`.

- The override matches by name and parameters. If both methods match, the non-const method wins.
- A native base with only a const method matches the override. The generated method is const.

```hot
class Foo : Widget, IConst {
    // Widget has `Size()` and IConst has `Size() const`.
    override int Size() => 2; // overrides the non-const method of Widget
}
```

A script class has one method for each name. The body can write the fields of `this`.

### Parameter forms of an override

A native virtual method can take a handle type in several forms. Write the plain handle type in the override. The translator finds the native method and keeps its signature. The body gets a handle.

| Native parameter | The body gets |
|---|---|
| `Weapon*`, `const Weapon*` | `Weapon` through `shared_from_this()`, or an intrusive reference |
| `Weapon&`, `const Weapon&` | the same |
| `std::shared_ptr<Weapon>`, `const std::shared_ptr<Weapon>&`, `std::shared_ptr<Weapon>&&` | the handle itself |

```hot
class Foo : Gun {
    // Gun has `virtual void Hit(Weapon* w)`.
    override void Hit(Weapon w) { w.dmg = 1; }
}
```

- An override copies the native parameter text. A plain parameter matches `T`. An `in T` parameter matches `const T&`. A `ref T` parameter matches `T&`. The default rule for plain parameters is for new methods only.
- The exact native spelling always works and wins. `override void Hit(Weapon* w)` is the exact form.
- If two native methods carry the same handle type, the override is ambiguous. The error lists each method. Write the exact parameter type.
- If no native method matches, the error `hides the virtual method` lists the candidates. A `.cpp` script also gets a hint with the native parameter type.
- A `std::shared_ptr` stub needs `std::enable_shared_from_this` in the native class to take a `T*` or a `T&`. Without it, the C++ compile stops with a message. Write the exact type instead.
- A `const Weapon*` or `const Weapon&` binds a normal handle. A script handle has no `const`: the translator infers it. The handle gives write access even when the native method passes `const`. Do not write to it.
- An intrusive stub (`intrusive("AddRef", "Release")`) adds a reference for a `T*`.
- A `T*` of an object that no `std::shared_ptr` owns throws `NullReference` in the override. A null `T*` gives a null handle.
- A `.cpp` script does not use this rule. It writes the exact native type.
- A native method that returns `std::shared_ptr<Weapon>` by value can be overridden with the return type `Weapon`. The slot keeps the native return type.
- A native return type `Weapon*`, `Weapon&` or an intrusive `Weapon*` is not relaxed: the lifetime of the result is not clear. Write the exact native type.
- A borrowed `T*` or `T&` in the body is not part of this rule.

## Script parent

A script class can derive from a script class. `virtual` opens a method for overrides. `base(...)` calls the parent constructor. `new` hides a parent method instead of replacing it.

```hot
using "Actor.h";

class Unit : Actor {
    int hp = 100;

    Unit(int start) : base(std.string("unit")) { hp = start; }
    virtual int Power() { return 1; }
    int Attack() { return Power() * 10; }
}

class Grunt : Unit {
    int rage = 3;

    Grunt(int start, int r) : base(start + 1) { rage = r; }
    override int Power() { return rage * 2; }
    new int Attack() { return base.Attack() + 1; }
}
```

## Abstract classes

`abstract class` marks a class that has abstract methods. The compiler does not allow `new` of an abstract class. The error is `cannot create an instance of the abstract class 'C'`. A host can still create the object with `_create()`. A child class that does not override all abstract methods stays abstract.

```hot
abstract class Shape {
    abstract int Area();
}

class Square : Shape {
    int side = 3;
    override int Area() { return side * side; }
}
```

A reload can make a live class abstract and back again. A live object keeps its state.

## Abstract methods

An abstract method has no body and the word `abstract`. It is virtual, so a child class implements it with `override`. A call of an abstract method that has no implementation throws `ScriptError` with the text `abstract method 'C.F' called`. The call `base.F()` of an abstract method is an error.

```hot
abstract class Shape {
    abstract int Area();
    int Twice() { return Area() * 2; }   // calls the slot of the child
}
```

A `.hot` class must override each pure virtual method of a native base, or it must be `abstract`. A pure virtual method with `noexcept` cannot throw. The call reports the error and does not end the program. If the result type has no default value (a reference), the host gives a `hot::noexcept_result<R>` specialization (see [Embedding rules](embedding-rules.html)).

## Parent in another file

`using "parent.hot";` makes the classes of another script visible. The script joins the generation of the child. A class name in two scripts is an error. A native stub in the main script wins over a stub with the same name in the imported script.

```hot
using "parent.hot";
using "Actor.h";

class Grunt : Unit {
    Grunt(int r) : base(r + 1) { }
}
```

For `is` and `as`, see [Statements](statements-text.html#is-and-as).

## Interfaces

An `interface` is a type with methods and no state. A class can implement many interfaces. A `.hot` script declares an interface at file level.

```hot
interface IShape {
    float Area();
    std.string Name();
}
```

A class lists the interfaces in its base list. The parent class comes first. The interfaces come after it. A class with no parent can start with an interface.

```hot
class Square : Actor, IShape {
    float side = 2.0;
    float Area() { return side * side; }
    std.string Name() { return "square"; }
}

class Disc : IShape {
    float Area() { return 3.0; }
    std.string Name() { return "disc"; }
}
```

### Implementation

The implementation is implicit. No `override` is needed. A public method implements an interface method. It must have the same name, the same parameters and the same return type. There is no covariance. A method with another signature is an error:

```text
class 'Sq' does not implement 'IShape.Scale(int)': the method 'Scale' has another signature
```

A missing method is also an error: `class 'Sq' does not implement 'IShape.Scale'`. These rules also apply:

- A method of the script parent can implement the method. A child class does not repeat the interface.
- A private method, a static method and an `async` method do not implement an interface method.
- An `abstract` method implements an interface method. An `abstract class` does not need all methods. Its child classes must implement them.
- A native virtual method that a script overrides implements an interface method only when the C++ types are equal. Otherwise add a script method that calls it.
- A struct cannot implement an interface.
- An interface cannot have a parent interface or a base list. It cannot have generics.
- An interface has methods only. It cannot have fields, properties, constructors or default methods.
- An interface cannot have `static` members, `async` methods or attributes.

### Use

A class handle converts to the interface without a cast. An interface value works in a field, a parameter, a return type and a collection such as `std.vector<IShape>`.

```hot
class Scene {
    std.vector<IShape> shapes;
    float Total() {
        float sum = 0.0;
        foreach (var s in shapes) sum += s.Area();
        return sum;
    }
    void Add() {
        IShape s = new $Disc();     // implicit conversion
        shapes.push_back(s);
        if (s is $Disc) { $Disc d = s as $Disc; }
        if (s is IShape) { }       // true when the class implements it
    }
}
```

`is I` and `as I` test a value for an interface. `i is C` and `i as C` test an interface value for a class. The operators `==` and `!=` compare the identity of the objects. An interface value and a class handle can be compared. `null` is a valid interface value.

A call of a method on a `null` interface value throws `ScriptError` with the text `null interface handle 'IShape'`.

The compiler gives an error for these uses:

| Use | Error |
|---|---|
| `new IShape()` | `cannot create an instance of the interface 'IShape'` |
| `[weak] IShape s;` | `[weak] cannot be used with the interface 'IShape': an interface handle is not a class handle. Remove '[weak]'` |
| `class C : IShape, X`: an interface before the parent class | `the interface 'IShape' cannot be the base class of 'C': list the base class 'X' first and the interfaces after it` |
| `class C : Actor, IShape, IShape`: the same interface twice | `'IShape' is listed twice` |
| `struct S : IShape` | `a struct cannot implement the interface 'IShape'` |
| An interface in the optimized AOT mode | `script interfaces are not supported in the optimized AOT mode` |

### Reload

An interface value is a handle to the object. It survives a reload. The call finds the method by name in the class of the object. So the call always uses the code of the current generation. The loader rebuilds the interface tables at each reload.

- A class can drop an interface. Stored values keep the object. Then `s is IShape` is false. A call on such a value throws `ScriptError` with the text `class 'C' does not implement 'IShape.Area'`.
- A class can be removed. A call on its objects is ignored, and the loader counts the call.
- A reload can change the signature of an interface method. It works when the interface and the implementing classes change together.
- A reload that changes the signature on one side only is a translate error. The old generation stays active.

The fixtures `tests/scripts/iface_v1.hot` and `tests/scripts/iface_params_v1.hot` show these cases.

### Run time data

The translator writes the table `iface_rows` in each class record. It lists the interface methods of the class with the slot of the implementation. The field `ifaces` lists the interface names. The loader builds `iface_slots` and `iface_slot_count` at each commit. The index of `iface_slots` is a session id of the method. A call uses `hot::call_iface`, and the call site caches the id. A removed class has no table.

### Limits

- Interfaces are only for `.hot` scripts. A `.cpp` script cannot declare or implement them (see [C++ script limits](cpp-limits.html)).
- The optimized AOT mode does not support interfaces. The translator reports an error.
- There is no interface inheritance, no generics and no default method.
