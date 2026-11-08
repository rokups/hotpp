---
layout: default
title: Hot reload
nav_order: 3
---

A reload swaps the code of a running object. The object keeps its field values.

## What a reload does

The host compiles the new script as a new generation. The commit runs at the frame boundary. If the compile or the reload fails, the old generation keeps running.

| Change | Result |
|---|---|
| Method body | LIVE |
| Add a method, a `virtual` or an `override` | LIVE. An old object can call a new method at once. |
| Change a method signature | LIVE. Native delegates bound to the old signature detach with a warning. |
| Add, remove or reorder a field | LIVE |
| Change a field initializer | LIVE. Existing objects keep their values. |
| Change a constructor | LIVE. Existing objects do not run it again. |
| Rename a field with `[alias]` | LIVE. The value moves. |
| Rename a field without `[alias]` | LIVE. The value is lost and the runtime warns. |
| Change a field type | LIVE. A `convert_` hook converts the value. Else the default value and a warning. |
| A field of a move-only type, for example `std.unique_ptr<T>` | LIVE. The value moves (see [Move-only fields](#move-only-fields)). |
| Change a script struct, enum or module variable | LIVE |
| Change the native base class or a native interface base | Full reload |
| Change the script parent class | Full reload |
| Change the shell layout, for example the first use of `this` as a handle | Full reload |
| Remove a function | LIVE. The slot becomes defunct (see below). |
| Change a native header, a native class layout or a native virtual signature | Full reload. The host triggers it. |

A `.cpp` script (see [C++ scripts](cpp-scripts.html)) follows the same table. It has no properties, no script virtual methods and no script-class handles. A `.cpp` reload is slower, because `hotc` does one more `libclang` parse. A normal build of the file has no state migration and no slots.

## Removed functions

A removed function keeps its slot as a defunct entry. The reload applies live. The slot stays defunct until a full restart. A fresh load (generation 1) resets all slots.

A call of a removed function throws `hot::ScriptError`. The message is "call of removed script function Cls::fn". A script caller can catch it. A host call of a thunk is a normal script error: the outermost call gives the error to the script error handler and returns a default result (zero, an empty string, a null handle). A host call inside another call lets the error pass to the caller. The runtime counts each call (`hot::removed_call_count()`) and logs the first call of each removed function on stderr.

A removed class is different. A call of a method of a removed class does not throw. It logs once: "call of a method of a removed script class or function ignored (it is no longer in the source); later calls are counted only". Then the runtime counts the calls and returns a default result (zero, an empty string, a null handle).

A function that returns with the same name reuses its slot. A function that returns with a new signature gets a new keyed thunk. The slot table grows by one entry for each distinct removed name until the restart. A parent virtual function that a child still overrides cannot be removed: this is a normal compile error ("'override' has no virtual method to override").

## Tasks

A task of an `async` method keeps its generation alive. After a reload, a suspended task of the old generation ends with `TaskReloadCancelled` at its next resume. The old code does not run. A retired generation unloads only after the host destroys the task. `Loader::reset` fails while a task lives, so destroy all tasks first. Call `hot::scheduler().cancel_all()` to destroy the spawned tasks before `reset`. See [Async and await](async.html).

## Full reload

A few changes cannot keep live objects. `Loader::request_reload` then returns the status `FullReloadRequired`, with a reason and the class names. The live generation stays. The host restarts all script state and keeps its own state.

A native base, a script parent or a shell layout change changes the C++ layout of the shell. A live object cannot move to a new layout. These changes stay full reloads. A removed function is not a cause: it reloads live.

The runtime does not see a change in a native header. When a native header changes, the host starts a full reload itself. `LoadResult::dependencies` lists the headers and the imported scripts that the build read: the host watches these files.

The host sequence:

1. Save the host state, for example the position and the name of each actor.
2. Drop all script objects, handles, closures and thunk pointers.
3. Call `loader.reset(error)`.
4. Compile the script with `backend->load(source, 1, hot::LoadMode::Fresh)`.
5. Call `loader.load(std::move(*fresh.module))`. The generation is 1 again.
6. Restore the host state with the new scripts.

```cpp
hot::ReloadResult r = loader.request_reload(backend->load(source, next_generation++));
if (r.status == hot::ReloadStatus::FullReloadRequired) {
    std::printf("[host] full reload: %s\n", r.error.c_str());
    std::string saved = world.Save();                 // 1
    world.Clear();                                    // 2
    std::string error;
    if (!loader.reset(error)) {
        std::printf("[host] full reload failed: %s\n", error.c_str());
        world.Restore(saved, make_actor);             // the old scripts go on
        return;
    }
    hot::LoadResult fresh =
        backend->load(source, 1, hot::LoadMode::Fresh);  // 4
    if (fresh && loader.load(std::move(*fresh.module))) {
        next_generation = 2;
        world.Restore(saved, make_actor);             // 6
    }
}
```

The sample [hot-reload](https://github.com/rokups/hotpp/blob/master/samples/hot-reload/main.cpp) shows `request_reload`, `commit_pending` and `frame_boundary` in a small program.

The host must drop all objects first. `reset` fails when one script object remains. The error names the classes, for example `3 live objects remain: Driver x1, G x1, S x1`. It also fails when a reload is pending or a closure pins a generation. A failed `reset` changes nothing. A module variable can hold a closure, for example in a `std.function`. The closure pins its generation, and `reset` then fails with `generation N is pinned (a closure or a task is alive)`. Before `reset`, the host calls a script function that clears such a variable (see `tests/scripts/modclosure.hot`). `loader.load` fails with `BadModule` when the loader still has a generation: call `reset` first. Build the `Fresh` module before `world.Clear()` when you can: a build error then leaves the actors intact (the test host `tests/engine/Harness.cpp` does this). After a `reset`, every pointer from `find()`, `module_state()` and `thunk()` is invalid.

## Renamed fields

`[alias("old")]` gives the old name. The reload moves the value to the new field.

```hot
using "Actor.h";

class Hero : Actor {
    [alias("hp")] float health = 100.0;
}
```

## Changed types

`convert_<field>` gets the old value and returns the new value. The function has no `this`. It cannot read other fields.

```hot
using "Actor.h";

class Hero : Actor {
    float level = 1.0;          // was: int level

    float convert_level(int old) {
        return old * 10.0;
    }
}
```

## Move-only fields

A class field can have a move-only type, for example `std.unique_ptr<T>` or `std.vector<std.unique_ptr<T>>`. A `.cpp` class has the same rule. The migration copies a field that can copy. It moves a field that cannot copy. The new object then owns the same heap objects. The old object keeps an empty value, and its destruction frees nothing.

- A `convert_` hook gets the old value of a move-only type by a move: `std.shared_ptr<int> convert_level(std.unique_ptr<int> old) { return std.shared_ptr<int>(std.move(old)); }`.
- In `onReload`, `old.get<T>(name)` of a moved field gives the empty value. Read the new field.
- A failed migration (a move constructor or an initializer throws) keeps the old objects. A moved field of an old object is then empty.
- A clone of an object with a move-only field throws `std::runtime_error` ("class C is not copyable: field 'f'").
- A `struct` field cannot have a move-only type, because a copy of a struct copies each field. The error is `a struct field of move-only type '...' is not supported`. Use a class.
- A `Task` field stays an error: a reload cancels a suspended task of the old code (see [Async](async.html#reload)).

## `onReload`

`void onReload(OldState old)` runs once for each live object, after the migration of all objects. `old.has("hp")` tests the old field name. `old.get<float>("hp")` reads the old value. It throws when the field is absent or has another type.

A throw from `onReload` is logged. The object keeps the defaults for the affected fields, and the reload completes. A reload has no rollback after the commit. To undo a change, revert the source. The next reload migrates back with the same rules.

```hot
using "Actor.h";

class Hero : Actor {
    [alias("hp")] float health = 100.0;
    float old_hp = 0.0;

    void onReload(OldState old) {
        if (old.has("hp")) old_hp = old.get<float>("hp");
        if (health > 100.0) health = 100.0;
    }
}
```

## Module variables

A module variable can have a `convert_<name>` hook at file level. It is the only free function that a script can have.

```hot
using "Actor.h";

float level = 1.0;              // was: int level

float convert_level(int old) {
    return old * 1.5;
}

class Probe : Actor {
    int seen = 0;
}
```
