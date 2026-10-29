---
layout: default
title: Hot reload
nav_order: 10
---

A reload swaps the code of a running object. The object keeps its field values.

## What a reload does

The host compiles the new script as a new generation. The commit runs at the frame boundary. If the compile or the reload fails, the old generation keeps running.

| Change | Result |
|---|---|
| Method body | LIVE |
| Add or remove a method, a virtual or an override | LIVE |
| Change a method signature | LIVE. Native delegates bound to the old signature detach with a warning. |
| Add, remove or reorder a field | LIVE |
| Change a field initializer | LIVE. Existing objects keep their values. |
| Change a constructor | LIVE. Existing objects do not run it again. |
| Rename a field with `[alias]` | LIVE. The value moves. |
| Rename a field without `[alias]` | LIVE. The value is lost and the runtime warns. |
| Change a field type | LIVE. A `convert_` hook converts the value. Else the default value and a warning. |
| Change a script struct, enum or module variable | LIVE |
| Change the native base class or a native interface base | REJECTED |
| Change the script parent class | REJECTED |
| Change the shell layout, for example the first use of `this` as a handle | REJECTED |
| Change a native header, a native class layout or a native virtual signature | RESTART |

## Renamed fields

`[alias("old")]` gives the old name. The reload moves the value to the new field.

```csharp
using "Actor.h";

class Hero : Actor {
    [alias("hp")] float health = 100.0;
}
```

## Changed types

`convert_<field>` gets the old value and returns the new value. The function has no `this`. It cannot read other fields.

```csharp
using "Actor.h";

class Hero : Actor {
    float level = 1.0;          // was: int level

    float convert_level(int old) {
        return old * 10.0;
    }
}
```

## onReload

`void onReload(OldState old)` runs once for each live object, after the migration of all objects. `old.has("hp")` tests the old field name. `old.get<float>("hp")` reads the old value. It throws when the field is absent or has another type.

```csharp
using "Actor.h";

class Hero : Actor {
    [alias("hp")] float health = 100.0;
    float old_hp = 0.0;

    void onReload(OldState old) {
        if (old.has("hp")) this.old_hp = old.get<float>("hp");
        if (this.health > 100.0) this.health = 100.0;
    }
}
```

## Module variables

A module variable can have a `convert_<name>` hook at file level. It is the only free function that a script can have.

```csharp
using "Actor.h";

float level = 1.0;              // was: int level

float convert_level(int old) {
    return old * 1.5;
}

class Probe : Actor {
    int seen = 0;
}
```
