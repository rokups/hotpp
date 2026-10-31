---
layout: default
title: C++ script classes
nav_order: 25
group: C++ scripts
---

This page describes what a script class can have in a `.cpp` script.

## What a script class can have

- A class that derives from a native class, or from a script class of the same file (see [Script class inheritance](cpp-types.html#script-class-inheritance)).
- Fields with initializers and migration.
- C array fields, for example `int data[4]`, `const int lut[3]`, `std::string names[2][3]` or `std::function<void()> handlers[4]`. A module variable and a static data member can also have an array type. The field keeps its array type in the state. A reload copies each element. An array that changes its type or size resets, and the loader writes a warning. The fixtures `tests/scripts/cpp_array_v1.cpp` and `tests/scripts/cpp_array_v2.cpp` show them. An array field cannot be in a member initializer list.
- `HOT_ALIAS("old")` on a field.
- `convert_<field>` methods.
- The method `onReload(const hot::OldState&)`.
- Constructors with a base initializer, for example `Actor(args)`, and with a member initializer list (see [Member initializer lists](cpp-classes.html#member-initializer-lists)).
- Destructors.
- Static data members and static methods (see [Static members](cpp-classes.html#static-members)).
- Methods, constructors and destructors that the file defines outside the class (see [Definitions outside the class](cpp-classes.html#definitions-outside-the-class)).
- `override` of native virtual methods, and native interface bases.
- New `virtual` methods, and `override` of these methods in a child class (see [Script class inheritance](cpp-types.html#script-class-inheritance)).
- Default arguments on a constructor and on a method. The text of a default gets the same edits as a method body, so it can name a module variable. An `override` has no default argument (hotc gives an error): the call through the base sees the base declaration. The fixture `tests/scripts/cpp_defaults.cpp` shows them.

At file scope a `.cpp` script can have:

- Module variables, with free-function `convert_<var>` hooks.
- Namespaces that hold script classes and module variables. See [Namespaces](cpp-translation.html#namespaces).
- Verbatim code: enums, structs without a base, free functions, `using`, namespaces without script code and `const` or `constexpr` variables.
- A free function, a function template, a `constexpr` function or a function in a namespace can use a module variable (see [How hotc edits a body](cpp-translation.html#how-hotc-edits-a-body)). A `constexpr` function can read it only in a call at runtime.

## Static members

A script class can have static data members and static methods. A normal C++ build compiles them as written. `hotc` moves them out of the class, because the generated class is a state struct with free functions.

A static data member is a module variable. Its name is `<Class>__s_<name>`. For `static int count;` in `Counter`, the name is `Counter__s_count`. The marker `s_` keeps the name apart from the names that `hotc` generates for the class, such as `Counter__plan`. So a static member can have the name `plan`. The module state holds the variable. It migrates by name and type, as any module variable does. The value survives a reload. A new static data member takes its initializer.

`hotc` accepts these forms of the initializer:

- `static inline int count = 10;` (C++17) in the class.
- `static int count;` in the class and `int Counter::count = 10;` at file scope. The file-scope text defines the module variable. `hotc` removes it from the output. Do not put an initializer in both places.

A static method is a free function with the name `<Class>__s_<name>`. It has no `__self`. `hotc` writes its prototype before the class block, so the methods of the class can call it. It writes the definition after the class block. The prototype keeps the default arguments. The definition has none. A static method has the edits of a free function: module variables and static members become `hot_mod()->...` or the mangled name. A static method that uses `this` or a member of the base class is an error of the C++ compiler (for example `call to non-static member function without an object argument`).

`hotc` rewrites every use of a static member in the file. These forms are valid:

- The bare name in the class, in a method or in another static method.
- `Counter::count`, `this->count`, `(*this).count` and `&Counter::count`.
- `obj.count` and `obj->count`. `hotc` drops the object, so the object must have no side effect. Else the message is `a static member must be used by its name`.
- Calls with arguments, for example `Counter::scale(3)`.
- The uses outside the class body, for example in a free function or in the initializer of a module variable. A module variable can read a static data member. It cannot call a static method: the prototype comes after the module block. A free function can do both.

The class name is also a type. See [Script classes as types](cpp-types.html#script-classes-as-types).

A static data member that is `const` or `constexpr` is not a module variable. It has no state. `hotc` writes it as a `const` at file scope before the class block, with the name `<Class>__s_<name>`. The uses in the class and outside it use this name. The member needs an initializer in the class. `static constexpr int kFactor = 3;` becomes `constexpr int Counter__s_kFactor = 3;`.

When the module block does not exist before the class, a static data member makes the module block at the class. So the types of the static data members must come before the class.

### Convert hooks of a static data member

A static method `convert_<name>` in the class is the hook of the static data member `<name>`. It works as the free function `convert_<var>` of a module variable. The generated hook is `convert_<Class>__s_<name>`. It converts the old value when the type changes. It cannot use a module variable, a static data member or a static method.

```cpp
class Counter : public Actor
{
    static float count; // was: static int count;
    static float convert_count(int old) { return old + 0.5f; }
};
float Counter::count = 10.0f;
```

The fixtures `tests/scripts/cpp_static_v1.cpp` and `tests/scripts/cpp_static_v2.cpp` show the forms. The second one changes the type of `count` from `int` to `float`.

## Definitions outside the class

A script class can declare a method, a constructor or a destructor and define it later in the file, as in plain C++.

```cpp
class Player : public Actor
{
public:
    Player(int a);
    int hit(int d, int scale = step);
    int get() const;
    int hp = 10;
};

class Other : public Actor { /* ... */ };

Player::Player(int a) : Actor("Player") { hp = a; }
int Player::hit(int d, int scale) { hp -= d * scale; return hp; }
int Player::get() const { return hp; }
```

The body of the definition has the same edits as a body in the class. It can use a native base member, `this`, a module variable and a field. The definition can come after other classes. The fixture `tests/scripts/cpp_outofclass_v2.cpp` shows the forms.

**Limit:** the body moves into the code of the class. So the body can use only what the class can use. A body that names a type, a function or a constant that the file declares after the class and before the definition is an error of `hotc` (see [Unsupported forms](cpp-limits.html#unsupported-forms)). Plain C++ allows it. Declare such a name before the class. A prototype before the class is enough for a function.

These rules apply:

- The class declaration gives the types, the `override`, the access and the default arguments. Do not repeat a default argument in the definition: C++ does not allow it.
- The body uses the parameter names of the definition. They can differ from the names in the declaration.
- A reload treats the method as a method with a body in the class. A change of the body, or a move of the body out of the class or into it, keeps the state of the objects.
- `hotc` removes the definition from the output and puts the body in the code of the class.
- A static method must have its body in the class (see [Static members](cpp-classes.html#static-members)).

## Member initializer lists

A constructor can have a member initializer list, as in plain C++. Both forms work, `f(e)` and `f{e}`, in a constructor in the class and in a constructor that the file defines outside the class.

```cpp
class Player : public Actor
{
public:
    Player(int hp_, std::string n) : Actor("Player"), hp(hp_ * 2), name(std::move(n)), tag{3} { stamp++; }
    int hp;
    std::string name;
    int tag;
    int stamp = 5;
};
```

These rules apply:

- The entries run before the body of the constructor, in the order of the field declarations, not in the order of the list. This is the rule of C++.
- An entry can use the parameters of the constructor and the fields that the declaration order puts before it. Such a field already has its new value.
- An entry builds the value as C++ does: `f(e)` is a direct initialization and `f{e}` is a list initialization. Then `hotc` moves the value into the field. So the type of the field must be move assignable.
- A field with a default member initializer and no entry keeps the default value.
- A field with an entry first gets its default value, and then the entry sets it. A default member initializer cannot read a field that has an entry in the list. The default would see the default value of that field, not the value from the list. `hotc` gives an error when the initializer names the field itself. It does not find a read through a function call, so avoid `int b = calc();` when `calc` reads a field that has an entry. Add the field of the default member initializer to the list.
- The entry of the base class, for example `Actor(args)`, has the handling that [What a script class can have](cpp-classes.html#what-a-script-class-can-have) describes.
- The text of an entry gets the same edits as a body. It can use a native base member, a module variable and a lambda.
- A reload does not run a constructor again for a live object. The state of the object stays. A new object runs the initializers of the new generation.

The fixtures `tests/scripts/cpp_ctor_init_v1.cpp` and `tests/scripts/cpp_ctor_init_v2.cpp` show the forms.

## Aliases, onReload and convert hooks

These features follow the rules of [Hot reload](hot-reload.html). Only the spelling is C++.

- `HOT_ALIAS("old")` before a field gives the old name of the field. On clang the macro is `[[clang::annotate("hot.alias=old")]]`. A compiler other than clang ignores it.
- `convert_<field>(Old old)` is a method of the class. It converts the old value to the new type of the field.
- `convert_<var>(Old old)` is a free function. It converts the old value of a module variable.
- `void onReload(const hot::OldState& old)` runs once for each live object, after the migration. Use `old.has("hp")` and `old.get<float>("hp")`.
