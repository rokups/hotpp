---
layout: default
title: C++ script types and inheritance
nav_order: 26
group: C++ scripts
---

This page describes a script class as a type and the inheritance between script classes in a `.cpp` script.

## Script classes as types

Script code can use a script class as a type. A method of another class, a free function and a module variable can hold a pointer or a reference to a script object. The fixtures `tests/scripts/cpp_objects_v1.cpp` and `tests/scripts/cpp_objects_v2.cpp` show the forms.

| Form | Meaning |
|---|---|
| `Minion*`, `Minion&`, `const Minion&` | A pointer or a reference to a script object. |
| `Minion& add(int v) { n += v; return *this; }` | A method can return `Minion&`, `const Minion&` or `Minion*` of a script class (fluent style). |
| `new Minion(3)`, `new Minion`, `new Minion{3}` | Makes a script object with the constructor of the script class. |
| `p->power`, `ref.power` | Reads or writes a field of the object. |
| `p->boost(2)` | Calls a method of the object. |
| `p->Tick(dt)` | Calls an `override` through the native virtual method. |
| `dynamic_cast<Minion*>(actor)` | Gives the object, or `nullptr` when `actor` is not a `Minion`. |
| `dynamic_cast<Minion&>(*actor)` | Gives the object. It throws `std::runtime_error` when `*actor` is not a `Minion`. |
| `static_cast<Minion*>(actor)`, `(Minion*)actor` | A cast without a check, as in C++. |
| `sizeof(Minion)`, `alignof(Minion)` | The size and the alignment of the object. |
| `delete p` | Destroys the object. The destructor of the script class runs. |

These rules apply:

- A pointer to a script object stays valid after a reload. The fields of the object migrate as the fields of any script object. A call after the reload runs the new code.
- A script class is not a borrowed type for a return value. A script object has a fixed address for its life, and a reload moves only the state block. So a method can return `P&`, `const P&` or `P*` of a script class, and `return *this;` is valid. The chain `a.add(1).add(2)` works before and after a reload. The fixtures `tests/scripts/cpp_fluent_v1.cpp` and `tests/scripts/cpp_fluent_v2.cpp` show it.
- The rule is only for one `*` or one `&`. A return of `P**` or `P*&` is an error. A return of a native `Mesh&` is still an error. A field of type `P&` is still an error.
- A call of a retired generation (a stale call) of a method that returns a reference throws. A stale call of a method that returns a pointer gives `nullptr`.
- A module variable can hold the pointer. The class must come before the first module variable.
- `dynamic_cast` to a script class works after a reload. It does not use the C++ RTTI of the script class. The test uses the name of the script class.
- A failed `dynamic_cast` to a reference throws `std::runtime_error`, not `std::bad_cast`.
- The script code owns an object that it makes with `new`. Delete it with `delete` through a pointer to the script class or to its native base. `hotc` does not delete it. A `.hot` script has no `delete`, because it uses handles.

A function template or a generic lambda (`auto` parameter) that gets a pointer to a script object can use only the native base members and the `override` methods of the object. Write `p->power` or `p->boost(2)` only in code that names the script class, for example `int power_of(Minion* p)`. `hotc` does not check the body of a template. A use of a script field or a script method there gives an error of the C++ compiler. Do not give a script field the name of a member of the native base: a template then uses the base member without an error.

A static method and a static data member can name their own class, for example `static Minion* spawn(int p)` and `inline static Minion* first_made = nullptr;`.

The size of a script object can change at a reload. So a script object cannot be a value: only a pointer or a reference can follow it. The C++ RTTI of a script class is not the same in two generations. So `typeid` of a script class is an error. Use `dynamic_cast`.

The `.hot` page [Statements](statements-text.html#is-and-as) compares these forms with `is`, `as` and `new` of a `.hot` script.

## Native pointer fields

A field or a module variable `T*` or `const T*` of a native class is stored as `hot::CheckedPtr<T>`. The class needs a weak source: a `std::enable_shared_from_this` base, a non-intrusive handle stub, or a `hot::weak_ref_traits` specialization. The fixture `tests/scripts/cpp_native_ptr_forms.cpp` shows the forms.

| Form | Meaning |
|---|---|
| `Charm* other = nullptr;` | A weak pointer. It reads `nullptr` after the object dies. |
| `other == nullptr`, `other != nullptr` | Tests the live target. |
| `other = nullptr;` | Clears the pointer. |
| `other = best();` | A raw `T*` from a native call. The class needs `std::enable_shared_from_this`, and a `std::shared_ptr` must own the object. Else the assignment throws `hot::NullReference`. |
| `other = h.get();` | The same, from a handle. A `.cpp` script cannot assign a handle to a `T*`, as in C++. |
| `other->Twice()`, `(*other).Twice()` | A call on the object. It throws `hot::NullReference` on a null or dead pointer. |
| `Polish(other)` | Passes the borrowed pointer to a native `T*` parameter. |
| `[this] { return other->Twice(); }` | A lambda reads the field through `this`. |

The pointer keeps its target across a reload. The module variable `Charm* g_other` works the same way.

A reference field, a pointer to a builtin type and a pointer to a class with no weak source are errors. See [C++ script limits and errors](cpp-limits.html). `delete other` is not supported.

## Script class inheritance

A script class can derive from another script class of the same file. The child class gets the fields and the methods of the parent. The fixtures `tests/scripts/cpp_inherit_v1.cpp` and `tests/scripts/cpp_inherit_v2.cpp` show the forms.

```cpp
class Base : public Actor
{
public:
    Base(int start = 0) : Actor("Base"), hits(start) {}
    virtual int Power() { return 1; }
    int Twice() { return Power() * 2; }
    int hits = 0;
};

class Kid : public Base
{
public:
    Kid(int start = 3) : Base(start * 2) {}
    int Power() override { return 5; }
    void Tick(float dt) override
    {
        Base::Tick(dt);
        hits += Twice() + Base::Power();
    }
};
```

These rules apply:

- The child derives from the parent with `public`. The parent is the only base of the child. The parent derives from a native class, or from a script class.
- The child code reads and writes the fields of the parent, and calls the methods of the parent, as in C++.
- A `virtual` method of a script class is a new virtual method. A child class overrides it with `override`. A call of the method from the parent code runs the override of the object.
- `Base::Power()` in the child calls the version of the parent, without the virtual call. This also works for a native virtual method, for example `Base::Tick(dt)`.
- `Base::` before a member that is not static works only on the object itself: `Base::Power()` or `this->Base::Power()`.
- The constructor of the child gives the arguments to the constructor of the parent, for example `Kid(int a) : Base(a * 2)`. Give all the parameters of a constructor of the parent: `hotc` does not use the default arguments of the parent here. A child constructor without the parent entry needs a parent constructor without parameters. Other counts give the error `'base(...)' has <n> arguments; ...`.
- A method of the child with the name of a method of the parent hides the method of the parent, as in C++.
- `this` in a parent method is the object of the child. `dynamic_cast<Kid*>(base_ptr)` finds the object, also after a reload.
- A new generation can add a field to the parent or to the child, add an `override`, or add a new `virtual` method. A live object keeps the values of its fields. The calls on the object run the new code after the reload.
