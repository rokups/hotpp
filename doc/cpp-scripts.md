---
layout: default
title: C++ scripts
nav_order: 17
---

A `.cpp` script is a plain C++ file. It uses the same hot reload as a `.hot` script. This page describes the `.cpp` script mode.

## What a `.cpp` script is

A `.cpp` script holds a class that derives from a native engine class. The class has fields and methods. The host gives the path of the file to the engine, as it does for a `.hot` script.

The extension selects the parser:

- `.hot` selects the custom S++ syntax.
- `.cpp` selects libclang.

Both modes work in one program. A `Loader` holds one module for each generation, as for `.hot` scripts. Use one session for each script. There is no import between the modes. A `.cpp` script cannot include a `.hot` script. A `.hot` script cannot import a `.cpp` script.

## The file compiles unchanged

A normal CMake build compiles a `.cpp` script with no `sppc`. Do not write `#include "hot/script.h"` in the script. The tool `sppc` injects `hot/script.h` itself. The header gives the macro `HOT_ALIAS` and the type `hot::OldState`.

The aids `HOT_ALIAS`, `onReload` and `convert_*` are temporary. Use them for a reload session. Remove them after the session. Then the file is standard C++ and a normal build compiles it with no extra option. The fixture `tests/scripts/cpp_player_v1.cpp` has no aids.

A normal build of a file that still has aids needs the header. Pass `-include hot/script.h` to the compiler, and add the `include` directory of spp to the include path. The fixture `tests/scripts/cpp_player_v2.cpp` has aids. Its plain target `cpp_scripts_plain2` does this.

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

- The field `hp` has the new name `health`. `HOT_ALIAS("hp")` keeps the value.
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

    void onReload(const hot::OldState& old)
    {
        if (old.has("hp") && old.get<float>("hp") > 100.0f)
            health = 100.0f;
    }

    HOT_ALIAS("hp") float health = 100.0f;
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
- In a lambda, a field or a method of the object goes through the object. See "Lambdas".
- `this` as an argument becomes the native owner.
- A non-const variable at file scope or in a namespace becomes a migrated module variable.
- A file-scope declaration can use a module variable. Examples are a free function, a function template, a `constexpr` function, a function in a namespace and a default argument. `sppc` edits the declaration like a method body: the name becomes `spp_mod()->name`. The declaration stays in its place. A declaration that uses no module variable stays as written.

The generated file keeps the source order of the script. A block at the top declares `spp_mod()`. The module block comes at the place of the first module variable. The block of a class comes at the place of the class. So a module variable can use only the types and the functions that come before the first module variable. A variable that is not a module variable cannot use a module variable: it gets its value when the library loads, before the module state exists.
- A static member or a nested type of the base gets the `Base::` prefix.
- A use of a script class as a type gets the edits in "Script classes as types".

All other text stays as you wrote it.

## Lambdas

A lambda in a method, in a constructor or in a field initializer can use the fields, the methods and the native base members of the object. Use any capture: `[this]`, `[=]`, `[&]`, `[=, this]`, `[&, this]` or an init-capture such as `[self = this]`. A generic lambda and a nested lambda work too.

A reload moves the fields of an object to a new place. So a lambda must not keep the address of the fields. `sppc` makes the lambda keep the object instead. The object keeps its address for all its life. Each use of a field in the lambda finds the fields at the time of the call. A lambda that a field holds sees the new values after a reload.

- `[this]` becomes a capture of the object. `[=, this]` becomes `[=]`.
- `[&]` keeps the object by copy. A lambda that you store and call after the method returns is safe if it uses only fields and methods. It still holds a reference to each local that it uses, so do not store a lambda that uses a local.
- In `[self = this]`, the name `self` is the object. `self->count` reads the field of the object.

```cpp
void Tick(float dt) override
{
    auto bump = [this] { count += 1; Move(Vec3(dt, 0, 0)); };
    bump();
    if (!later)
        later = [&] { return count + 1000; }; // the field "later" is a std::function<int()>
}
```

The lambda in a field has the code of the version that made it. A reload does not replace the body. The old version stays loaded while a live field can hold code. The next reload does not unload the old version. A lambda that you make again after the reload has the new code. To get the new code, set the field again.

A lambda cannot capture `*this`. A copy of the object has no meaning in a script. `sppc` gives an error. Capture a copy of the fields, for example `[n = count]`.

## What a script class can have

- A class that derives from a native class, or from a script class of the same file (see "Script class inheritance").
- Fields with initializers and migration.
- `HOT_ALIAS("old")` on a field.
- `convert_<field>` methods.
- The method `onReload(const hot::OldState&)`.
- Constructors with a base initializer, for example `Actor(args)`, and with a member initializer list (see "Member initializer lists").
- Destructors.
- Static data members and static methods (see "Static members").
- Methods, constructors and destructors that the file defines outside the class (see "Definitions outside the class").
- `override` of native virtual methods, and native interface bases.
- New `virtual` methods, and `override` of these methods in a child class (see "Script class inheritance").
- Default arguments on a constructor and on a method. The text of a default gets the same edits as a method body, so it can name a module variable. An `override` has no default argument (sppc gives an error): the call through the base sees the base declaration. The fixture `tests/scripts/cpp_defaults.cpp` shows them.

At file scope a `.cpp` script can have:

- Module variables, with free-function `convert_<var>` hooks.
- Namespaces that hold script classes and module variables. See "Namespaces".
- Verbatim code: enums, structs without a base, free functions, `using`, namespaces without script code and `const` or `constexpr` variables.
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
- The script code owns an object that it makes with `new`. Delete it with `delete` through a pointer to the script class or to its native base. `sppc` does not delete it. A `.hot` script has no `delete`, because it uses handles.

A function template or a generic lambda (`auto` parameter) that gets a pointer to a script object can use only the native base members and the `override` methods of the object. Write `p->power` or `p->boost(2)` only in code that names the script class, for example `int power_of(Minion* p)`. `sppc` does not check the body of a template. A use of a script field or a script method there gives an error of the C++ compiler. Do not give a script field the name of a member of the native base: a template then uses the base member without an error.

A static method and a static data member can name their own class, for example `static Minion* spawn(int p)` and `inline static Minion* first_made = nullptr;`.

The size of a script object can change at a reload. So a script object cannot be a value: only a pointer or a reference can follow it. The C++ RTTI of a script class is not the same in two generations. So `typeid` of a script class is an error. Use `dynamic_cast`.

The `.hot` page [Statements](statements.html#is-and-as) compares these forms with `is`, `as` and `new` of a `.hot` script.

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
- The constructor of the child gives the arguments to the constructor of the parent, for example `Kid(int a) : Base(a * 2)`. Give all the parameters of a constructor of the parent: `sppc` does not use the default arguments of the parent here. A child constructor without the parent entry needs a parent constructor without parameters. Other counts give the error `'base(...)' has <n> arguments; ...`.
- A method of the child with the name of a method of the parent hides the method of the parent, as in C++.
- `this` in a parent method is the object of the child. `dynamic_cast<Kid*>(base_ptr)` finds the object, also after a reload.
- A new generation can add a field to the parent or to the child, add an `override`, or add a new `virtual` method. A live object keeps the values of its fields. The calls on the object run the new code after the reload.

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

- `HOT_ALIAS("old")` before a field gives the old name of the field. On clang the macro is `[[clang::annotate("hot.alias=old")]]`. A compiler other than clang ignores it.
- `convert_<field>(Old old)` is a method of the class. It converts the old value to the new type of the field.
- `convert_<var>(Old old)` is a free function. It converts the old value of a module variable.
- `void onReload(const hot::OldState& old)` runs once for each live object, after the migration. Use `old.has("hp")` and `old.get<float>("hp")`.

## Namespaces

A script class and a module variable can be in a named namespace. The compact form `namespace game::ai { ... }` works too. A normal C++ build compiles the file as written.

```cpp
namespace game
{
int score = 0;
int bonus(int x) { return x + score; }

class Player : public Actor
{
public:
    Player() : Actor("Player") {}
    void Tick(float dt) override { score += 1; points = bonus(10); }
    int points = 0;
};
} // namespace game

namespace game::ai
{
class Brain : public Player { /* ... */ };
} // namespace game::ai
```

- A class in a namespace keeps its qualified name in the hot-reload state. The class above is `game::Player`. The class `Brain` is `game::ai::Brain`. So `red::Unit` and `blue::Unit` are two different classes. A reload migrates each one on its own.
- A module variable in a namespace has the namespace in its name, with `__` between the parts. The variable `game::score` is `game__score`. The variable `game::ai::mood` is `game__ai__mood`. This name is the key of the migration. A tool that reads the module state shows this name.
- A convert hook of a module variable in a namespace is a free function at file scope, not in the namespace. Its name is `convert_` and the name with `__`. For `game::score` the hook is `convert_game__score`. The hook of a variable at file scope does not change: `convert_total`.
- A function in a namespace stays in the namespace. It can use a module variable of the namespace by its short name or by a qualified name. A class can use the names of its namespace without a qualifier.
- Name lookup works as in C++ for `using namespace`, a using-declaration, a namespace alias and a qualified name like `game::Player`. A class at file scope can use a script class of a namespace by any of these forms.
- A class of an inner namespace can derive from a class of an outer namespace.
- A namespace with no script class and no module variable stays as written. It has no migration. An `extern "C"` block also stays as written, so a variable in it is not a module variable.
- `HOT_ALIAS` is for a field only. It has no effect on a class name, so a class in a namespace has no alias. A rename of the class or of its namespace makes a new class. It is not a migration.
- A class and a variable in different namespaces must not give the same name after the join with `__`. The class `game::Player` and a class `game__Player` give the same name, and `sppc` gives an error.

## Unsupported forms

A class with no base is not a script class. `sppc` copies it unchanged, like a struct without a base. It has no hot reload.

The first version gives an error for each form in this list. The text in the second column is the message of `sppc`.

| Form | Message |
|---|---|
| A script class or a module variable in an anonymous namespace | `an anonymous namespace cannot hold a script class or a module variable in a .cpp script; use a named namespace` |
| A script class or a module variable in an inline namespace | `an inline namespace cannot hold a script class or a module variable in a .cpp script; remove 'inline'` |
| A convert hook for a module variable of a namespace, written inside a namespace | `the convert hook of the module variable '<ns::var>' must be at file scope and must be named 'convert_<ns__var>'` |
| Two classes or two module variables with the same name after the join with `__` | `'<a>' and '<b>' have the same generated name '<name>'. Rename one of them` |
| A script class that derives from a class of the file that is not a script class | `a .cpp script class cannot derive from '<name>': it is a plain class of the script; derive from a script class or from a native class` |
| A script class that derives from a script class of another `.cpp` file | `a .cpp script class cannot derive from '<name>' of another .cpp file; a script class can derive only from a script class of the same file` |
| A class in a function or in another class that derives from a script class | `the class '<name>' cannot derive from the script class '<parent>': only a class at file scope can derive from a script class in a .cpp script` |
| A script class with a script parent and one more base class | `a .cpp script class with the base the script class '<name>' cannot have another base class` |
| A script parent with `virtual`, for example `class Kid : public virtual Base` | `a .cpp script class cannot derive from the script class '<name>' with 'virtual'` |
| A script parent with `protected` or `private` | `a .cpp script class must derive from the script class '<name>' with 'public'` |
| `virtual` without `override` on a method that overrides a virtual method | `the method '<name>' overrides a virtual method; write 'override'` |
| A static member used through an object that has a side effect, for example `f().count` | `a static member must be used by its name` |
| An instance method `s_x` and a static member `x` in one class (both get the name `<Class>__s_x`) | `the method '<Class>::s_x' and the static member '<Class>::x' have the same generated name '<Class>__s_x'. Rename one of them` |
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
| A script class by value, for example `Minion m;` or a parameter `Minion m` | `the script class '<name>' by value is not supported in a .cpp script; use a pointer or a reference` |
| An array of a script class, for example `Minion a[2];` | `an array of the script class '<name>' is not supported in a .cpp script; use a pointer or a reference` |
| A script class as a template argument, for example `std::vector<Minion>` | `the script class '<name>' as a template argument is not supported in a .cpp script; use a pointer or a reference` |
| `typeid` of a script class or of a script object | `'typeid' of the script class '<name>' is not supported in a .cpp script: a script class has no C++ RTTI; use dynamic_cast` |
| A placement `new` of a script class | `a placement 'new' of the script class '<name>' is not supported in a .cpp script` |
| An array `new` of a script class, for example `new Minion[2]` | `an array 'new' of the script class '<name>' is not supported in a .cpp script; use an array of pointers` |
| `Minion::` before a member that is not static, on another object or outside a child class, for example `p->Minion::boost(1)` | `the script class '<name>' can qualify only a static member, or a member that a child class uses on itself, in a .cpp script` (outside a script class the end of the message is `can qualify only a static member in a .cpp script`) |
| A forward declaration of a script class | `a forward declaration of the script class '<name>' is not supported in a .cpp script; define the class before its first use` |
| A lambda that captures `*this` | `a lambda cannot capture '*this' in a .cpp script: it would copy the object; capture a copy of the fields` |
| A module variable that uses a type of the file that comes after the first module variable | `the type '<type>' must come before the first module variable '<name>': the module block is there and needs the complete type` |
| A module variable whose initializer uses a function of the file that comes after the first module variable | `'<function>' must come before the first module variable '<name>': the module block is there and holds the initializers` |
| A variable that is not a module variable and uses a module variable in its initializer | `the variable '<variable>' cannot use the module variable '<name>' in a .cpp script: it gets its value before the module state exists` |
| A base member hidden by a macro | `a macro hides a member of the base class; write the member in the script text` |
| An include of a `.hot` script | `a .cpp script cannot include a .hot script` |

For a `const` field or a reference field, use a default member initializer in the class body. For a delegating constructor, write the shared code in a method.

## Known limits

- A field of a native struct type that has a function pointer can point into the code of an old version. `sppc` finds such a struct in an included header, wherever the `#include` is. Then the old version stays loaded while a live object has the field. `sppc` does not find a struct that you define in the script file itself.
- An in-class brace initializer of a class-type field is not visited by libclang. A module variable inside such an initializer is not rewritten.
- A `.cpp` translation does one more libclang parse than a `.hot` translation. A reload is slower. One parse of the engine headers takes about 400 ms.
- In a normal build the file has plain C++ meaning. There is no state migration and there are no slots. Objects must not be shared between the two builds.
- The language server gives only the errors of `sppc` for a `.cpp` script, and only when a host lists the file. The other editor features come from clangd. For a script with aids, clangd needs the flags `-include hot/script.h` and `-I<spp>/include`. See [Tools](tools.html#a-cpp-script-in-the-editor).

## CMake

`spp_add_scripts` takes `.cpp` paths in `SCRIPTS`, as it takes `.hot` paths. A `foo.hot` and a `foo.cpp` in one call fail at configure time, because they have the same stem. See [CMake](cmake.html).

```cmake
spp_add_scripts(TARGET gen1 SCRIPTS cpp_player_v1.cpp)
spp_add_scripts(TARGET gen2 SCRIPTS cpp_player_v2.cpp AFTER gen1 SPPC_ARGS --gen 2)
```
