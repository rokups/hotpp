---
layout: default
title: Reload API
nav_order: 31
group: Embedding
---

## Reload

**Links:** `hot_backend` (or any backend), or `hot_runtime` with prebuilt generations.

Ask for a reload at any time. Commit it at the frame boundary.

```cpp
// next: a LoadResult of generation 2, from a backend
hot::ReloadResult r = loader.request_reload(std::move(next));   // checks only
// later, at the frame boundary of the main thread:
loader.commit_pending();
loader.frame_boundary();                                        // unloads old generations
```

`request_reload` has two overloads. One takes a `Module`. The other takes a `LoadResult`: a failed result becomes a `ReloadResult` without a call of the validation. A backend failure with the reason `SlotsRemoved` becomes `FullReloadRequired`. This is a safety net: the translator does not produce this reason. Any other failure becomes `Failed`.

`request_reload` returns a `hot::ReloadResult` (`include/hot/runtime/reload_result.h`). It has a `status`, a `reason`, an `error` text and the `classes` involved. The status is one of these:

| Status | Meaning |
|---|---|
| `Applied` | The new generation is active. |
| `Pending` | The module is acceptable. `commit_pending` applies it. |
| `FullReloadRequired` | Hot reload cannot apply the change. The host does a full reload. |
| `Failed` | The module is not acceptable. The live generation continues. |

The result converts to `true` for `Applied` and `Pending`. The reason is one of `None`, `NativeBaseChanged`, `InterfaceBasesChanged`, `ScriptParentChanged`, `ShellLayoutChanged`, `SlotsRemoved` (a safety net only), `CompileError`, `BadModule`, `GenerationNotNewer`, `Busy` and `MigrationFailed`. `to_string` gives the name.

`commit_pending` returns `Applied` when it committed. With nothing pending it returns `Failed` with the reason `None` and an empty error. It returns `Busy` when the commit cannot run now, and `MigrationFailed` when the state does not migrate. If the runtime cannot go on (for example a native move constructor throws during a migration), it calls `hot::request_restart`. This calls the handler from `set_restart_handler` and aborts when there is none. When the handler returns, the commit fails, the old generation stays active and `restart_requested()` is true.

**Run time:** the object keeps its field values. The new code runs through the same pointer. The retired generation unloads in `frame_boundary`.

**Requires:** a backend at run time (or prebuilt generations). Each generation number must be higher than the active one, else the reason is `GenerationNotNewer`.

**Failure:** read `status`, `reason` and `error`. For `Failed`, the old generation goes on. For `FullReloadRequired`, do a full reload (below).

**Sample:** [hot-reload](https://github.com/rokups/hotpp/tree/master/samples/hot-reload). See also [Hot reload](hot-reload.html) for the changes that a reload accepts.

### Visit the live objects

`Loader::each_object(visit)` calls `visit` once for each live script object. The argument is the `hot::ShellBase*` of the object. The record of the object is `self->cls`. Its canonical record is `self->cls->canonical`, or `self->cls` when that is null. The order is not fixed.

Call it on the main thread only, for example after `commit_pending`. The walk holds the lock of the list of live objects. Thus `visit` must not run script code, and it must not create or destroy a script object. To run script code on each object, collect the objects first. Then call the code after the walk.

A host uses it after a reload, for example to subscribe each object of a changed class again. The test `EachObjectVisitsLiveObjectsAfterCommit` (`tests/integration/reload/hook_tests.cpp`) shows the use.

### Full reload

For `FullReloadRequired`, the host does these steps:

1. Save the host state.
2. Drop all script objects, handles, closures and thunk pointers.
3. Call `loader.reset(error)` on the same `Loader`.
4. Call `backend->load(source, 1, hot::LoadMode::Fresh)`.
5. Call `loader.load(module)`.
6. Restore the host state.

The host does the serialization. The script has no hook for it. `reset` returns `false` and changes nothing when objects remain. The error is, for example, `2 live objects remain: Player x1, Weapon x1`. It also fails when a reload is pending or a generation is pinned. The check for a call from script code is exact only in `EntryCounters` mode. The host must not call `reset` from script code. After `reset`, pointers from `find()`, `module_state()` and `thunk()` are invalid. The runtime does not see a change in a native header: the host starts a full reload itself. `LoadResult::dependencies` lists the files that the build read other than the script. The host watches them, and a change needs a full reload. `tests/engine/Harness.cpp` (`full_reload`) shows the full reload sequence. It is a test host: for a user project, start from `samples/hot-reload`.
