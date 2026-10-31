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

## Enums and structs

An enum is a scoped enum. Add a base type after `:`. The default base type is `int`. A struct is a value type. It is the only way to make one: `[value]` on a script class is an error.

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
