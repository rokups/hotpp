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
using Soldier = Recruit;           // a script class: a handle, like Recruit

class Platoon {
    using Count = int;             // in a class body: for this class only
    Ints nums;
    Soldier lead;                  // null at the start, new Soldier() makes an object
    Count n;
}
```

- The translator replaces the alias name by its target when it reads the script. A script class alias has the handle storage of the class. A native type alias is that type.
- A target can use an alias that the script declares before it. A later alias and a cycle (`using A = B; using B = A;`) are errors.
- An alias in a class body hides a module alias with the same name, in that class.
- The C++ text has the alias too (as the storage type of the target), for the C++ code of a stub.
- These are errors: an alias that uses itself (`using A = A;`), a name that an alias declares twice in one scope, and an alias with the name of a class, struct or enum. The name of a language type (`int`, `uint`, `byte`, `Task`...) or a C++ keyword is also an error.
- Do not give an alias the name of a variable. A name before a `.` is an alias first.
- An imported script keeps its own aliases: they are not visible in the importing script. The output has one file scope for all scripts, so an alias name must not be the name of a class, enum or stub of any script. Two scripts can declare the same alias with the same type. With two types it is an error.
- Editor support: hot-ls and the vscode grammar do not know `using namespace` and `using Name = type;` yet. They show the lines as unknown.

## Enums and structs

An enum is a scoped enum. Add a base type after `:`. The default base type is `int`. A struct is a value type. `[value]` on a class definition is an error. `[value]` on a use of a class copies it: see [Value uses](types-handles.html#value-uses).

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

An initializer can use only a variable that is above it. A later variable, or the variable itself, is an error. The name `HotModule` is reserved.

`[alias("old")]` before a variable keeps the value of the old name.

```hot
[alias("total")] int sum = 0;
```

## Native stubs

A stub tells the translator how a native type behaves. `handle` makes it a shared handle. `value` makes it a copied value.

```hot
using "Actor.h";
using "Weapon.h";

[native, handle("std::shared_ptr<$T>")] class Weapon;
[native, value] class std.string;
```

A stub can cover only the subclasses of a type, and `cpp_prelude` adds C++ text: see "Scope of a stub" in [native-interop-scope.md](native-interop-scope.md).

A script has no free functions. Only a convert hook (`T convert_<var>(Old old)`) is allowed at file level. A convert hook cannot read a module variable. An imported script cannot have module variables or convert hooks.
