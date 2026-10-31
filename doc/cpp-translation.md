---
layout: default
title: How hotc edits C++
nav_order: 24
group: C++ scripts
---

This page describes the edits that `hotc` makes in a `.cpp` script: method bodies, lambdas and namespaces.

## How hotc edits a body

`hotc` parses the `.cpp` file with libclang. It keeps each method body as text. It edits only the names that the generated code cannot resolve:

- A bare use or a `this->` use of a native base member goes to the native owner. Examples are `Move(...)` and `this->Name`.
- In a lambda, a field or a method of the object goes through the object. See [Lambdas](cpp-translation.html#lambdas).
- `this` as an argument becomes the native owner.
- A non-const variable at file scope or in a namespace becomes a migrated module variable.
- A file-scope declaration can use a module variable. Examples are a free function, a function template, a `constexpr` function, a function in a namespace and a default argument. `hotc` edits the declaration like a method body: the name becomes `hot_mod()->name`. The declaration stays in its place. A declaration that uses no module variable stays as written.

The generated file keeps the source order of the script. A block at the top declares `hot_mod()`. The module block comes at the place of the first module variable. The block of a class comes at the place of the class. So a module variable can use only the types and the functions that come before the first module variable. A variable that is not a module variable cannot use a module variable: it gets its value when the library loads, before the module state exists.
- A static member or a nested type of the base gets the `Base::` prefix.
- A use of a script class as a type gets the edits in [Script classes as types](cpp-types.html#script-classes-as-types).

All other text stays as you wrote it.

## Lambdas

A lambda in a method, in a constructor or in a field initializer can use the fields, the methods and the native base members of the object. Use any capture: `[this]`, `[=]`, `[&]`, `[=, this]`, `[&, this]` or an init-capture such as `[self = this]`. A generic lambda and a nested lambda work too.

A reload moves the fields of an object to a new place. So a lambda must not keep the address of the fields. `hotc` makes the lambda keep the object instead. The object keeps its address for all its life. Each use of a field in the lambda finds the fields at the time of the call. A lambda that a field holds sees the new values after a reload.

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

A lambda cannot capture `*this`. A copy of the object has no meaning in a script. `hotc` gives an error. Capture a copy of the fields, for example `[n = count]`.

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
- A class and a variable in different namespaces must not give the same name after the join with `__`. The class `game::Player` and a class `game__Player` give the same name, and `hotc` gives an error.
