---
layout: default
title: C++ scripts
nav_order: 17
---

A `.cpp` script is a plain C++ file. It uses the same hot reload as a `.spp` script. This page describes the `.cpp` script mode.

## What a `.cpp` script is

A `.cpp` script holds a class that derives from a native engine class. The class has fields and methods. The host gives the path of the file to the engine, as it does for a `.spp` script.

The extension selects the parser:

- `.spp` selects the custom S++ syntax.
- `.cpp` selects libclang.

Both modes work in one program. A `Loader` holds one module for each generation, as for `.spp` scripts. Use one session for each script. There is no import between the modes. A `.cpp` script cannot include a `.spp` script. A `.spp` script cannot import a `.cpp` script.

## The file compiles unchanged

A normal CMake build compiles a `.cpp` script with no `sppc`. Do not write `#include "spp/script.h"` in the script. The tool `sppc` injects `spp/script.h` itself. The header gives the macro `SPP_ALIAS` and the type `spp::OldState`.

The aids `SPP_ALIAS`, `onReload` and `convert_*` are temporary. Use them for a reload session. Remove them after the session. Then the file is standard C++ and a normal build compiles it with no extra option. The fixture `tests/scripts/cpp_player_v1.cpp` has no aids.

A normal build of a file that still has aids needs the header. Pass `-include spp/script.h` to the compiler, and add the `include` directory of spp to the include path. The fixture `tests/scripts/cpp_player_v2.cpp` has aids. Its plain target `cpp_scripts_plain2` does this.

In a normal build the file has plain C++ meaning. There is no state migration and there are no slots. Do not share objects between the two builds.

## Example

The file `tests/scripts/cpp_player_v1.cpp` is generation 1.

```cpp
// Generation 1 of the Player class as a plain C++ script.
// A normal C++ build compiles this file. The tool sppc also translates it.
#include "Actor.h"
#include "Math.h"

#include <cstdio>
#include <string>

enum class Mode { Idle, Run };

struct Point
{
    float x = 0, y = 0;
};

// A module variable. It keeps its value across a reload.
int total = 0;
const float kMaxHp = 100.0f;

static float clamp_hp(float v)
{
    return Math::Max(Math::Min(v, kMaxHp), 0.0f);
}

class Player : public Actor
{
public:
    Player() : Actor("Player") {}

    void Tick(float dt) override
    {
        Move(Vec3(speed * dt, 0, 0));
        std::printf("[v1] %s: Tick dt=%.4f pos.x=%.4f hp=%.1f\n", this->Name.c_str(), dt, Position.x, hp);
        heal(1.0f);
        total++;
        auto step = [&] { Move(Vec3(0, 1, 0)); };
        step();
    }

    void TakeDamage(float amount) override
    {
        hp = clamp_hp(hp - amount);
        std::printf("[v1] %s: TakeDamage %.1f -> hp=%.1f\n", this->Name.c_str(), amount, hp);
    }

    void TakeDamage(Actor& source, float amount) override
    {
        target = source.Name;
        std::printf("[v1] %s: hit by %s\n", this->Name.c_str(), target.c_str());
        TakeDamage(amount);
    }

    void heal(float a)
    {
        hp = clamp_hp(hp + a);
        std::printf("[v1] %s: heal %.1f -> hp=%.1f\n", this->Name.c_str(), a, hp);
    }

    float hp = 100.0f;
    float speed = 5.0f;
    std::string target;
    Point p{1, 2};
    Mode mode = Mode::Idle;
    int level = 1;
};
```

The file `tests/scripts/cpp_player_v2.cpp` is generation 2. It makes these changes:

- The field `hp` has the new name `health`. `SPP_ALIAS("hp")` keeps the value.
- The field `level` changes from `int` to `float`. The method `convert_level` converts the value.
- The module variable `total` changes from `int` to `float`. The free function `convert_total` converts the value.
- The method `onReload` clamps `health`.
- The method `Tick` moves twice as fast.

```cpp
// Generation 2 of the Player class as a plain C++ script.
// The field hp has the new name health. The field level changes from int to float.
// The module variable total changes from int to float.
#include "Actor.h"
#include "Math.h"

#include <cstdio>
#include <string>

enum class Mode { Idle, Run };

struct Point
{
    float x = 0, y = 0;
};

float total = 0;
const float kMaxHp = 100.0f;

static float clamp_hp(float v)
{
    return Math::Max(Math::Min(v, kMaxHp), 0.0f);
}

// The convert hook of the module variable total.
float convert_total(int old)
{
    return old * 2.0f;
}

class Player : public Actor
{
public:
    Player() : Actor("Player") {}

    void Tick(float dt) override
    {
        Move(Vec3(2.0f * speed * dt, 0, 0));
        std::printf("[v2] %s: Tick dt=%.4f pos.x=%.4f health=%.1f level=%.1f\n", this->Name.c_str(), dt,
            Position.x, health, level);
        total++;
        auto step = [&] { Move(Vec3(0, 1, 0)); };
        step();
    }

    void TakeDamage(float amount) override
    {
        health = clamp_hp(health - amount);
        std::printf("[v2] %s: TakeDamage %.1f -> health=%.1f\n", this->Name.c_str(), amount, health);
    }

    void TakeDamage(Actor& source, float amount) override
    {
        target = source.Name;
        std::printf("[v2] %s: hit by %s\n", this->Name.c_str(), target.c_str());
        TakeDamage(amount);
    }

    void heal(float a)
    {
        health = clamp_hp(health + 2.0f * a);
        std::printf("[v2] %s: heal %.1f (x2) -> health=%.1f\n", this->Name.c_str(), a, health);
    }

    // The convert hook of the field level.
    float convert_level(int old) { return old * 10.0f; }

    void onReload(const spp::OldState& old)
    {
        if (old.has("hp") && old.get<float>("hp") > 100.0f)
            health = 100.0f;
    }

    SPP_ALIAS("hp") float health = 100.0f;
    float speed = 5.0f;
    std::string target;
    Point p{1, 2};
    Mode mode = Mode::Idle;
    float level = 1.0f;
};
```

## How sppc edits a body

`sppc` parses the `.cpp` file with libclang. It keeps each method body as text. It edits only the names that the generated code cannot resolve:

- A bare use or a `this->` use of a native base member goes to the native owner. Examples are `Move(...)` and `this->Name`.
- `this` as an argument becomes the native owner.
- A non-const file-scope variable becomes a migrated module variable.
- A file-scope declaration can use a module variable. Examples are a free function, a function template, a `constexpr` function, a function in a namespace and a default argument. `sppc` edits the declaration like a method body: the name becomes `spp_mod()->name`. The declaration stays in its place. A declaration that uses no module variable stays as written.

The generated file keeps the source order of the script. A block at the top declares `spp_mod()`. The module block comes at the place of the first module variable. The block of a class comes at the place of the class. So a module variable can use only the types and the functions that come before the first module variable. A variable that is not a module variable cannot use a module variable: it gets its value when the library loads, before the module state exists.
- A static member or a nested type of the base gets the `Base::` prefix.
- A use of a script class as a type gets the edits in "Script classes as types".

All other text stays as you wrote it.

## What a script class can have

- A class that derives from a native class.
- Fields with initializers and migration.
- `SPP_ALIAS("old")` on a field.
- `convert_<field>` methods.
- The method `onReload(const spp::OldState&)`.
- Constructors with a base initializer, for example `Actor(args)`, and with a member initializer list (see "Member initializer lists").
- Destructors.
- Static data members and static methods (see "Static members").
- Methods, constructors and destructors that the file defines outside the class (see "Definitions outside the class").
- `override` of native virtual methods, and native interface bases.
- Default arguments on a constructor and on a method. The text of a default gets the same edits as a method body, so it can name a module variable. An `override` has no default argument (sppc gives an error): the call through the base sees the base declaration. The fixture `tests/scripts/cpp_defaults.cpp` shows them.

At file scope a `.cpp` script can have:

- Module variables, with free-function `convert_<var>` hooks.
- Verbatim code: enums, structs without a base, free functions, `using`, namespaces and `const` or `constexpr` variables.
- A free function, a function template, a `constexpr` function or a function in a namespace can use a module variable (see "How sppc edits a body"). A `constexpr` function can read it only in a call at runtime.

## Static members

A script class can have static data members and static methods. A normal C++ build compiles them as written. `sppc` moves them out of the class, because the generated class is a state struct with free functions.

A static data member is a module variable. Its name is `<Class>__s_<name>`. For `static int count;` in `Counter`, the name is `Counter__s_count`. The marker `s_` keeps the name apart from the names that `sppc` generates for the class, such as `Counter__plan`. So a static member can have the name `plan`. The module state holds the variable. It migrates by name and type, as any module variable does. The value survives a reload. A new static data member takes its initializer.

`sppc` accepts these forms of the initializer:

- `static inline int count = 10;` (C++17) in the class.
- `static int count;` in the class and `int Counter::count = 10;` at file scope. The file-scope text defines the module variable. `sppc` removes it from the output. Do not put an initializer in both places.

A static method is a free function with the name `<Class>__s_<name>`. It has no `__self`. `sppc` writes its prototype before the class block, so the methods of the class can call it. It writes the definition after the class block. The prototype keeps the default arguments. The definition has none. A static method has the edits of a free function: module variables and static members become `spp_mod()->...` or the mangled name. A static method that uses `this` or a member of the base class is an error of the C++ compiler (for example `call to non-static member function without an object argument`).

`sppc` rewrites every use of a static member in the file. These forms are valid:

- The bare name in the class, in a method or in another static method.
- `Counter::count`, `this->count`, `(*this).count` and `&Counter::count`.
- `obj.count` and `obj->count`. `sppc` drops the object, so the object must have no side effect. Else the message is `a static member must be used by its name`.
- Calls with arguments, for example `Counter::scale(3)`.
- The uses outside the class body, for example in a free function or in the initializer of a module variable. A module variable can read a static data member. It cannot call a static method: the prototype comes after the module block. A free function can do both.

The class name is also a type. See "Script classes as types".

A static data member that is `const` or `constexpr` is not a module variable. It has no state. `sppc` writes it as a `const` at file scope before the class block, with the name `<Class>__s_<name>`. The uses in the class and outside it use this name. The member needs an initializer in the class. `static constexpr int kFactor = 3;` becomes `constexpr int Counter__s_kFactor = 3;`.

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

**Limit:** the body moves into the code of the class. So the body can use only what the class can use. A body that names a type, a function or a constant that the file declares after the class and before the definition is an error of `sppc` (see "Unsupported forms"). Plain C++ allows it. Declare such a name before the class. A prototype before the class is enough for a function.

These rules apply:

- The class declaration gives the types, the `override`, the access and the default arguments. Do not repeat a default argument in the definition: C++ does not allow it.
- The body uses the parameter names of the definition. They can differ from the names in the declaration.
- A reload treats the method as a method with a body in the class. A change of the body, or a move of the body out of the class or into it, keeps the state of the objects.
- `sppc` removes the definition from the output and puts the body in the code of the class.
- A static method must have its body in the class (see "Static members").

## Script classes as types

Script code can use a script class as a type. A method of another class, a free function and a module variable can hold a pointer or a reference to a script object. The fixtures `tests/scripts/cpp_objects_v1.cpp` and `tests/scripts/cpp_objects_v2.cpp` show the forms.

| Form | Meaning |
|---|---|
| `Minion*`, `Minion&`, `const Minion&` | A pointer or a reference to a script object. |
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
- A module variable can hold the pointer. The class must come before the first module variable.
- `dynamic_cast` to a script class works after a reload. It does not use the C++ RTTI of the script class. The test uses the name of the script class.
- A failed `dynamic_cast` to a reference throws `std::runtime_error`, not `std::bad_cast`.
- The script code owns an object that it makes with `new`. Delete it with `delete` through a pointer to the script class or to its native base. `sppc` does not delete it. A `.spp` script has no `delete`, because it uses handles.

A function template or a generic lambda (`auto` parameter) that gets a pointer to a script object can use only the native base members and the `override` methods of the object. Write `p->power` or `p->boost(2)` only in code that names the script class, for example `int power_of(Minion* p)`. `sppc` does not check the body of a template. A use of a script field or a script method there gives an error of the C++ compiler. Do not give a script field the name of a member of the native base: a template then uses the base member without an error.

A static method and a static data member can name their own class, for example `static Minion* spawn(int p)` and `inline static Minion* first_made = nullptr;`.

The size of a script object can change at a reload. So a script object cannot be a value: only a pointer or a reference can follow it. The C++ RTTI of a script class is not the same in two generations. So `typeid` of a script class is an error. Use `dynamic_cast`.

The `.spp` page [Statements](statements.html#is-and-as) compares these forms with `is`, `as` and `new` of a `.spp` script.

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
- An entry builds the value as C++ does: `f(e)` is a direct initialization and `f{e}` is a list initialization. Then `sppc` moves the value into the field. So the type of the field must be move assignable.
- A field with a default member initializer and no entry keeps the default value.
- A field with an entry first gets its default value, and then the entry sets it. A default member initializer cannot read a field that has an entry in the list. The default would see the default value of that field, not the value from the list. `sppc` gives an error when the initializer names the field itself. It does not find a read through a function call, so avoid `int b = calc();` when `calc` reads a field that has an entry. Add the field of the default member initializer to the list.
- The entry of the base class, for example `Actor(args)`, has the handling that "What a script class can have" describes.
- The text of an entry gets the same edits as a body. It can use a native base member, a module variable and a lambda.
- A reload does not run a constructor again for a live object. The state of the object stays. A new object runs the initializers of the new generation.

The fixtures `tests/scripts/cpp_ctor_init_v1.cpp` and `tests/scripts/cpp_ctor_init_v2.cpp` show the forms.

## Aliases, onReload and convert hooks

These features follow the rules of [Hot reload](hot-reload.html). Only the spelling is C++.

- `SPP_ALIAS("old")` before a field gives the old name of the field. On clang the macro is `[[clang::annotate("spp.alias=old")]]`. A compiler other than clang ignores it.
- `convert_<field>(Old old)` is a method of the class. It converts the old value to the new type of the field.
- `convert_<var>(Old old)` is a free function. It converts the old value of a module variable.
- `void onReload(const spp::OldState& old)` runs once for each live object, after the migration. Use `old.has("hp")` and `old.get<float>("hp")`.

## Unsupported forms

A class with no base is not a script class. `sppc` copies it unchanged, like a struct without a base. It has no hot reload.

The first version gives an error for each form in this list. The text in the second column is the message of `sppc`.

| Form | Message |
|---|---|
| A class that derives from another class of the same file | `a .cpp script class cannot derive from a class of the same file (first version)` |
| `virtual` without `override` | `'virtual' is not supported in a .cpp script; use 'override' of a native virtual` |
| A static member used through an object that has a side effect, for example `f().count` | `a static member must be used by its name` |
| A static const member without an initializer in the class | `a static const member needs an initializer in the class` |
| A method, a constructor or a destructor that the class declares and the file never defines | `the method 'Player::hit' is declared but never defined; define it in the class or after the class` (the words `constructor` and `destructor` replace `method` for those members) |
| A definition outside the class that has no declaration in the class | `out-of-line definition of 'hit' does not match any declaration in 'Player'` (this error comes from the C++ parser) |
| A static method that is declared in the class and defined outside it | `define the static method inside the class` |
| A body outside the class that names a declaration which the file puts after the class | `the definition of 'Player::hit' uses 'kLate', which the file declares after the class; the body moves into the class, so declare 'kLate' before the class` |
| A pure virtual method (`= 0`) | `a pure virtual method is not supported in a .cpp script class` |
| A constructor, a destructor or a method that is `= default` or `= delete` | `a defaulted or deleted method is not supported in a .cpp script class` (the word `constructor` replaces `method` for a constructor) |
| A `const` field or a reference field in the initializer list | `the const field '<name>' cannot be in the initializer list: a field lives in the state; use a default member initializer` (the words `reference field` replace `const field` for a reference) |
| An array field in the initializer list | `the array field '<name>' cannot be in the initializer list; assign its elements in the body` |
| A delegating constructor, for example `Player() : Player(1) {}` | `a delegating constructor is not supported in a .cpp script class` |
| A default member initializer that reads a field which has an entry in the list | `the default member initializer of the field '<b>' reads the field '<a>', which this constructor sets in its initializer list; ...` |
| An entry in the initializer list that is not a field of the class, or a field that has two entries | An error of the C++ parser, for example `member initializer 'x' does not name a non-static data member or base class` |
| Any other declaration in the class | `this declaration is not supported in a .cpp script class` |
| Properties | There is no property syntax in C++. Use two methods. |
| Script-class handles | A script class is not a handle in a `.cpp` script. Use a pointer. See "Script classes as types". |
| Script inheritance | See "A class that derives from another class of the same file". |
| A script class by value, for example `Minion m;` or a parameter `Minion m` | `the script class '<name>' by value is not supported in a .cpp script; use a pointer or a reference` |
| An array of a script class, for example `Minion a[2];` | `an array of the script class '<name>' is not supported in a .cpp script; use a pointer or a reference` |
| A script class as a template argument, for example `std::vector<Minion>` | `the script class '<name>' as a template argument is not supported in a .cpp script; use a pointer or a reference` |
| `typeid` of a script class or of a script object | `'typeid' of the script class '<name>' is not supported in a .cpp script: a script class has no C++ RTTI; use dynamic_cast` |
| A placement `new` of a script class | `a placement 'new' of the script class '<name>' is not supported in a .cpp script` |
| An array `new` of a script class, for example `new Minion[2]` | `an array 'new' of the script class '<name>' is not supported in a .cpp script; use an array of pointers` |
| `Minion::` before a member that is not static, for example `p->Minion::boost(1)` | `the script class '<name>' can qualify only a static member in a .cpp script` |
| A forward declaration of a script class | `a forward declaration of the script class '<name>' is not supported in a .cpp script; define the class before its first use` |
| A `[this]` lambda that uses a base member | `a lambda that uses a base member must capture with [&] or [=]` |
| A module variable that uses a type of the file that comes after the first module variable | `the type '<type>' must come before the first module variable '<name>': the module block is there and needs the complete type` |
| A module variable whose initializer uses a function of the file that comes after the first module variable | `'<function>' must come before the first module variable '<name>': the module block is there and holds the initializers` |
| A variable that is not a module variable and uses a module variable in its initializer | `the variable '<variable>' cannot use the module variable '<name>' in a .cpp script: it gets its value before the module state exists` |
| A base member hidden by a macro | `a macro hides a member of the base class; write the member in the script text` |
| An include of a `.spp` script | `a .cpp script cannot include a .spp script` |

For a `[this]` lambda, use `[&]` or `[=]`. For a `const` field or a reference field, use a default member initializer in the class body. For a delegating constructor, write the shared code in a method.

## Known limits

- An in-class brace initializer of a class-type field is not visited by libclang. A module variable inside such an initializer is not rewritten.
- A `.cpp` translation does one more libclang parse than a `.spp` translation. A reload is slower. One parse of the engine headers takes about 400 ms.
- In a normal build the file has plain C++ meaning. There is no state migration and there are no slots. Objects must not be shared between the two builds.
- The language server gives only the errors of `sppc` for a `.cpp` script, and only when a host lists the file. The other editor features come from clangd. For a script with aids, clangd needs the flags `-include spp/script.h` and `-I<spp>/include`. See [Tools](tools.html#a-cpp-script-in-the-editor).

## CMake

`spp_add_scripts` takes `.cpp` paths in `SCRIPTS`, as it takes `.spp` paths. A `foo.spp` and a `foo.cpp` in one call fail at configure time, because they have the same stem. See [CMake](cmake.html).

```cmake
spp_add_scripts(TARGET gen1 SCRIPTS cpp_player_v1.cpp)
spp_add_scripts(TARGET gen2 SCRIPTS cpp_player_v2.cpp AFTER gen1 SPPC_ARGS --gen 2)
```
