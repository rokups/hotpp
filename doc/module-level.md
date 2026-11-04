---
layout: default
title: Module level
nav_order: 11
group: Hot script
---

The top of a script file can hold imports, enums, structs, variables and native stubs.

## Imports

`using "x.h";` includes a native header. `using <x>;` includes a standard header. `using "x.hot";` loads another script. A `using` can stand anywhere at module level, also after a class.

```hot
using "parent.hot";            // script
using "Actor.h";               // native header
using <algorithm>;             // standard header

class Grunt : Unit {
    Grunt(int r) : base(r + 1) { }
}

using <map>;                   // allowed here too
```

These headers are always included, so they need no `using`:
`<cstddef>`, `<cstdint>`, `<cstdio>`, `<cstdlib>`, `<cstring>`, `<functional>`, `<memory>`, `<mutex>`, `<new>`, `<stdexcept>`, `<string>`, `<tuple>`, `<type_traits>`, `<typeinfo>`, `<unordered_map>`, `<utility>` and `<vector>`. Any other standard header (`<map>`, `<optional>`, `<algorithm>`) needs a `using`.

### Namespaces

`using name;` makes the names of a native namespace visible. It becomes `using namespace name;` in the C++ text. A `.` in the name is a `::`. The line is at file scope, after all the includes, so it is valid for all the code of the script. The type lookup of the translator sees the names too.

```hot
using "game.h";
using game.ai;                 // using namespace game::ai;
using std;                     // the standard namespace (see the module attribute std)

class Grunt : Unit {
    Brain brain;               // game::ai::Brain
}
```

The name `std` follows the configured standard namespace, as `std.` does in a type.

### Type aliases

`using Name = type;` gives a new name to a type. It becomes `using Name = type;` in the C++ text. The name works everywhere a type name works: fields, parameters, return types, locals, template arguments, `new` and `as`. Before a `.` it is a scope, so an alias of an enum can name the values (`Color2.Red`).

```hot
using Ints = std.vector<int>;      // a native type
using Soldier = $Recruit;          // a script class: a handle, like $Recruit

class Platoon {
    using Count = int;             // in a class body: for this class only
    Ints nums;
    Soldier lead;                  // a handle: null at the start
    Count n;
}
```

- The translator replaces the alias name by its target when it reads the script. An alias is the type that it names: `$Recruit` is a handle and `Recruit` is a value.
- A target can use an alias that the script declares before it. A later alias and a cycle (`using A = B; using B = A;`) are errors.
- An alias in a class body hides a module alias with the same name, in that class.
- The C++ text has the alias too (as the storage type of the target), for the C++ code of a stub.
- These are errors: an alias that uses itself (`using A = A;`), a name that an alias declares twice in one scope, and an alias with the name of a class, struct or enum. The name of a language type (`int`, `uint`, `byte`, `Task`...) or a C++ keyword is also an error.
- Do not give an alias the name of a variable. A name before a `.` is an alias first.
- An imported script keeps its own aliases: they are not visible in the importing script. The output has one file scope for all scripts, so an alias name must not be the name of a class, enum or stub of any script. Two scripts can declare the same alias with the same type. With two types it is an error.
- Editor support: hot-ls and the vscode grammar do not know `using namespace` and `using Name = type;` yet. They show the lines as unknown.

## Enums and structs

An enum is a scoped enum. The C++ spelling `enum class Mode { ... }` and `enum struct Mode { ... }` means the same, so `class` is optional. Add a base type after `:`. The default base type is `int`. A struct is a value type. A bare class name is a value, and `$C` is a handle: see [Value uses](types-handles.html#value-uses).

```hot
enum Mode { Idle, Walk, Run }
enum Small : byte { A, B = 5, C }

struct Point {
    float x = 0.0;
    float y = 0.0;
}
```

## Module variables

A variable at file level is shared by all classes of the file. A reload keeps its value.

```hot
using "Actor.h";

enum Mode { Idle, Walk, Run }

int total = 0;
Mode mode = Mode.Idle;

class Hero : Actor {
    override void Tick(float dt) {
        total++;
        mode = Mode.Run;
    }
}
```

An initializer can use only a variable that is above it. A later variable, or the variable itself, is an error. An initializer cannot call a function, a free function or a static method. The error is `a module variable initializer cannot call the function '<name>'`. The name `HotModule` is reserved.

`[alias("old")]` before a variable keeps the value of the old name.

```hot
[alias("total")] int sum = 0;
```

A module variable `T*` of a native class with a weak source is a `hot::CheckedPtr<T>`, as a pointer field is. It is weak, it reads `null` after the object dies, and it keeps its target across a reload. The same errors apply as for a field. See "Pointer fields" in [Borrowing](borrowing.html).

```hot
using "Actor.h"; using "Equip.h";

Charm* last_charm = null;       // a checked pointer

class Hero : Actor {
    override void Tick(float dt) {
        if (last_charm != null) last_charm.Bump(1);
    }
}
```

## Free functions

A function at file level has no class. Write the return type, the name and the body.

```hot
using "Actor.h";

int Add(int a, int b) {
    return a + b;
}

int Scale(int x, int k = 3) {
    return x * k;
}

class Hero : Actor {
    int sum = 0;

    override void Tick(float dt) {
        sum = Add(2, Scale(4));
    }
}
```

A free function can have default parameter values. It can call itself and it can call other functions. It can read and write a module variable. It has no `this`.

The translator writes a free function as a plain C++ `static` function with the name `hot_fn_<name>`. It has no slot, no thunk and no manifest entry. A reload replaces the code of the module. The next call runs the new body.

Rules:

- A function name is unique. There are no overloads. The error is `function '<name>' is declared twice (overloads are not supported)`.
- A function cannot have the name of a module variable, a class, an enum, a struct or an interface. The error is `'<name>' is declared twice (a function and a module variable)`, with the kind in the brackets.
- A free function has no access word. It cannot be `static` or `async`.
- A function is not a value. `var f = Add;` is the error `'Add' is a function; call it, or write a lambda`.
- A module variable initializer and a convert hook cannot call a function. The error of a hook is `a convert hook cannot call the function '<name>': the new module state is not complete yet`.
- A field initializer cannot call a function. The error is `a field initializer cannot call the function '<name>': the prototype of the function comes after the class`.
- A default argument cannot call a function. The error is `a default argument cannot call the function '<name>': the prototype of the function comes after the declaration`.
- An imported script cannot have free functions. The error is `an imported script cannot have free functions`.
- There is no namespace block. Use `using ns;` for a native namespace.
- The host cannot call a free function by name through `Loader::thunk`. Call a method of a class.

A static method of a class is a function of the class. See [Static methods](classes.html#static-methods).

## Native stubs

A stub tells the translator how a native type behaves. `handle` makes it a shared handle. `value` makes it a copied value.

```hot
using "Actor.h";
using "Weapon.h";

[native, handle("std::shared_ptr<$T>")] class Weapon;
[native, value] class std.string;
```

A stub can cover only the subclasses of a type, and `cpp_prelude` adds C++ text: see "Scope of a stub" in [native-interop-scope.md](native-interop-scope.md).

A convert hook (`T convert_<var>(Old old)`) is a special function at file level. A convert hook cannot read a module variable. An imported script cannot have module variables, convert hooks or free functions.
