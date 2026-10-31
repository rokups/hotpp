---
layout: default
title: Rules, errors and traceback
nav_order: 29
group: Embedding
---

## Rules for all situations

### Ownership and lifetime

- The `Loader` owns every `Module`. A module owns its code memory (`keep_alive`): for a `.so` the last copy calls `dlclose`.
- Declare the `Loader` before the script objects. The loader is then destroyed after them. The objects must die before the loader unloads the code. All samples do this.
- Declare the backend before the loader. The samples do this too.
- `Loader::find(name)` returns a `const ClassEntry*` or `nullptr`. `entry->create()` makes an object and returns an `Instance`. `Instance::native` is a pointer to the native base class (for example `Creature`). The caller owns the object. Delete it through the native pointer (the native class needs a virtual destructor). The samples use `std::unique_ptr<Creature>`.
- With AOT, `AotRegistry::create(name)` returns the `Instance` and `create_as<Native>(name)` returns the native pointer. The caller owns the object, or gives it to the engine (for example `World::Add`).
- The pointers from `find()`, `module_state()` and `thunk()` and every function pointer, closure and handle that came from a script module become invalid after `Loader::reset`.
- A retired generation unloads in `frame_boundary()` when its pin count is zero, at least one frame boundary passed, and no code of the module is in use. A live state field that can hold code (`kMayHoldCode`, for example a `std::shared_ptr`) blocks every unload. A native struct with a function pointer field also sets `kMayHoldCode`. The translator finds the struct by its class or alias name, not a template instance such as `Cb<int>`.
- A JIT `Module` keeps the JIT alive, so its code stays valid. For `ClangFilesBackend` the module keeps the loaded library alive.

### Threads

These rules come from the comments in `include/hot/runtime/loader.h`.

- `load`, `frame_boundary`, `stats`, `find` and `thunk` run on the main thread only. Worker threads do not call them.
- `request_reload` and `reload_pending` can run on any thread, also while workers run script code.
- In the default mode (`StopTheWorld`), `commit_pending` runs on the main thread at the frame boundary, when no worker is in script code.
- In `EntryCounters` mode (`Loader::set_sync_mode`), `commit_pending` can run on one thread while other threads call script code. It blocks new script entries and waits for the running ones. It fails when the calling thread is in script code. The mode is global for the process. Change it only when no thread is in script code.
- In `EntryCounters` mode, native code that reads the fields of a script object holds a `ScriptEntry`. A script frame that waits for a commit, or for a thread at the gate, deadlocks.
- `Loader::reset` runs on the main thread when no worker runs script code. Never call it from script code.
- Only one thread at a time creates or destroys script objects. This includes `new` in script code, `ClassEntry::create` and the release of the last handle. A host that does this on more than one thread uses its own lock (see [Internals](internals.html#threads-and-the-safe-point)).
- `AotRegistry` is made and destroyed on the main thread.
- `ClangFilesBackend` builds one script at a time (it has a mutex for the shared slot file).

### Symbol export

A host that loads a module must export its symbols. The modules resolve the host functions and the runtime functions from the executable.

- Set `ENABLE_EXPORTS` on the executable (`-rdynamic` on Linux). Every sample except `aot` does this.
- Link a static library of your own as a whole archive when scripts call functions that the host code does not use. `samples/imgui/CMakeLists.txt` uses `$<LINK_LIBRARY:WHOLE_ARCHIVE,imgui_sdl3>` for this.
- The executable must link the runtime objects that the script code uses (see the JIT section of `README.md`). The samples link the Hot++ libraries without a whole-archive link.
- An AOT program does not load a module, so it does not export. A program with run-time overrides loads modules, so it exports (`MODE AOT_RELOAD` sets this).
- The runtime keeps the call depth of the script calls in a static TLS variable (`hot::detail::g_call_depth`). The script code reads it with one instruction. A host that is one executable, or that links the runtime library at the program start, needs nothing more. A host that loads the runtime library later with `dlopen` can fail with `cannot allocate memory in static TLS block`. Then give glibc more static TLS space with an environment variable. This needs glibc 2.32 or later. Set the variable before the process starts: a change at run time has no effect. Use 4096 bytes or more:

  ```sh
  GLIBC_TUNABLES=glibc.rtld.optional_static_tls=4096 ./my_host
  ```

### C++17 baseline

The generated code, `include/hot` and the runtime library need C++17 only. A C++17 host works. The standard of the scripts is the standard of the host (17, 20 or 23). A standard below 20 selects `fmt` for `${}`. See [CMake](cmake-options.html#the-format-library) for the table.

### Core copies

`module_from_library` makes a second, independent instance of the library for the loader (the "core"). It copies the file to the temp folder as `hot-core-<pid>-<n>-<name>` and removes the copy after `dlclose`. If the temp folder is not available, it uses the folder of the library. If the copy fails (`cannot copy <path> for the core module: ...`), the generation stays loaded and the loader prints `[script] WARNING: no core module for ...` to `stderr`.

### Error reports

A script can throw `NullReference`, `IndexOutOfRange` or `ScriptError` (see [Statements](statements.html)). All three derive from `hot::ScriptError`, which derives from `std::runtime_error`. Include `hot/runtime.h`.

When the host calls a script function and the script does not catch a `ScriptError`, the runtime reports it. Then the call returns a default value (`0`, `false`, or nothing for `void`). The object stays alive. The next call works. A field keeps the values that the script wrote before the throw.

Set a handler to receive the report:

```cpp
hot::set_script_error_handler([](const hot::ScriptErrorReport& r) {
    log("%s in %s: %s", r.type, r.class_name, r.message);
});
```

`hot::set_script_error_handler({})` restores the default. `hot::script_error_handler()` gives the current handler. The handler runs on the thread of the call. These fields are valid only during the call of the handler:

| Field | Meaning |
|---|---|
| `type` | `hot::NullReference`, `hot::IndexOutOfRange` or `hot::ScriptError`. |
| `message` | The text of `what()`. |
| `class_name` | The script class of the object, or `?`. |
| `self` | The shell of the object (`const hot::ShellBase*`), or null. |
| `traceback` | The script call stack of the throw point, or an empty text when the traceback is off. See "Traceback". |

Without a handler, the runtime prints one line to `stderr`: `[script] ERROR: unhandled hot::IndexOutOfRange in Player: <message>`. When the traceback is on, the lines of the traceback follow it.

An error reports only when it leaves the outermost script call. A script that calls native code, which calls a script override, can catch the error in the calling script.

Only a `ScriptError` stops at the boundary. Any other C++ exception that a script throws reaches the host. The host must catch it. If the return type has no default value (for example a reference), the runtime reports the error and throws it again.

The checks are a build-time choice. The CMake option `HOT_RUNTIME_CHECKS` (default `ON`) sets the macro `HOT_RUNTIME_CHECKS`. With `OFF` the macro is `0`: the generated code has no checks, and an installed host sees the same setting. Each check costs one compare for a pointer access, a handle access or an index of a checked container (`std::array`, `std::vector`, a stub with `checked_index`). See [CMake](cmake-options.html).

### Traceback

The traceback is off by default. To turn it on, configure Hot++ with `-DHOT_TRACEBACK=ON`. The option sets the macro `HOT_TRACEBACK` to `1` for the runtime library and for the compile of the scripts. It works for `.hot` scripts only.

With the traceback on, each script member function records a frame on a thread-local stack. Each statement stores its script line in the frame. A `hot::ScriptError` copies the stack when the script throws it, because the unwind removes the frames. `hot::ScriptError::traceback()` gives the text. `hot::ScriptErrorReport::traceback` has the same text. Each frame is one line, and the innermost frame is first:

```
[script] ERROR: unhandled hot::NullReference in Player: null handle dereference
  at Player.Inner (player.hot:12)
  at Player.Outer (player.hot:17)
  at Player.Tick (player.hot:21)
```

The stack keeps the 64 innermost frames. A last line `  ... N more` shows how many outer frames it dropped. A lambda records no frame and no line.

The cost is one store for each statement and one push and pop for each call. The generated code is the same when the option is `OFF`: the macros `HOT_TRACE_FRAME` and `HOT_TRACE_LINE` are empty. Build the host and the scripts with the same setting. A script without the macros gives an empty traceback.
