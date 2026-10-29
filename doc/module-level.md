---
layout: default
title: Module level
nav_order: 6
---

The top of a script file can hold imports, enums, structs, variables and native stubs.

## Imports

`using "x.h";` includes a native header. `using <x>;` includes a standard header. `using "x.spp";` loads another script.

```csharp
using "parent.spp";            // script
using "Actor.h";               // native header
using <string>;                // standard header

class Grunt : Unit {
    Grunt(int r) : super(r + 1) { }
}
```

## Enums and structs

An enum is a scoped enum. Add a base type after `:`. A struct is a `[value] class`.

```csharp
enum Mode { Idle, Walk, Run }
enum Small : byte { A, B = 5, C }

struct Point {
    float x = 0.0;
    float y = 0.0;
}
```

## Module variables

A variable at file level is shared by all classes of the file. A reload keeps its value.

```csharp
using "Actor.h";

enum Mode { Idle, Walk, Run }

int total = 0;
Mode mode = Mode.Idle;

class Hero : Actor {
    override void Tick(float dt) {
        total = total + 1;
        mode = Mode.Run;
    }
}
```

`[alias("old")]` before a variable keeps the value of the old name.

```csharp
[alias("total")] int sum = 0;
```

## Native stubs

A stub tells the translator how a native type behaves. `handle` makes it a shared handle. `value` makes it a copied value.

```csharp
using "Actor.h";
using "Weapon.h";

[native, handle("std::shared_ptr<$T>")] class Weapon;
[native, value] class std.string;
```

A script has no free functions. Only a convert hook (`T convert_<var>(Old old)`) is allowed at file level.
