---
layout: default
title: Backend interface and options
nav_order: 34
group: Embedding
---

## The backend interface

`include/hot/backend.h`, `include/hot/module.h`.

| Type | Role |
|---|---|
| `Backend` | The abstract class. `load(source, generation, mode)` makes the `Module` of one script for one generation. `name()` gives `CLANG_FILES` or `JIT`. |
| `ScriptSource` | The input of `load`. |
| `LoadMode` | `Reload` (default) continues the slot numbering. `Fresh` ignores the earlier slot state and builds a library with a new name. A full reload uses `Fresh` after `Loader::reset`. |
| `LoadResult` | A `Module` (`module`), or an error. |
| `Module` | A loadable generation. |
| `Loader` | Registers modules, reloads, unloads. |
| `ClassEntry` | `record` (the class record), `create` (a factory that returns an `Instance`) and `construct` (a constructor with arguments, see [Constructors with arguments](#constructors-with-arguments)). |

Fields of `ScriptSource`:

| Field | Meaning |
|---|---|
| `name` | The script name or path. The diagnostics and the `#line` directives use it. The output name uses its file stem. |
| `text` | The content of the script. |
| `modules` | The imported scripts (`using "parent.hot";`) as pairs of the path in the `using` line and the text. One generation holds them all. A path that is not here is read from disk. |

Fields of `LoadResult`:

| Field | Meaning |
|---|---|
| `module` | `std::optional<Module>`. `operator bool` is true when it has a value. |
| `error` | The diagnostics or the error text. Empty on success. |
| `reason` | A `ReloadReason`. A backend failure is `CompileError`. `SlotsRemoved` means that the loader found fewer slots in the new record. It is a safety net: the translator does not produce it. Only a full reload applies the change. Default `None`. |

Fields of `Module`:

| Field | Meaning |
|---|---|
| `register_fn` | `void (*)(Registry&)`. The `script_register` function. |
| `keep_alive` | `shared_ptr<void>`. The code stays valid while a copy exists. |
| `origin` | A name for messages, for example the library path. |
| `make_core` | A function that makes a second instance of the code (the core). Empty: the module cannot make one. |
| `on_accept` | The loader calls it when the module is live. A backend uses it to write the slot file only for a live module. |

`Backend::load` does not touch the live generation. On failure the result has an `error` and a `reason`.

`hot::module_from_library(path)` builds a `Module` from a shared library (`dlopen` and `dlsym`). The loader calls `Loader::load` for the first module, `request_reload` and `commit_pending` for the next ones, and `reset` for a full reload. `Loader` also has `stats()`, `stats_text()`, `set_sync_mode`, `sync_mode`, `reload_pending`, `find`, `module_state` and `thunk`.

### Constructors with arguments

`include/hot/runtime/migration.h`, `include/hot/runtime/records.h`.

`ClassEntry::create` makes an object with the default constructor. It gives no arguments. `ClassEntry::construct` gives arguments to a script constructor. The host can then give, for example, an engine context to the object.

```cpp
const hot::ClassEntry* entry = loader.find("Rotator");
hot::Instance obj = entry->construct(context); // calls Rotator(Urho3D.Context* context)
Actor* actor = loader.create_as<Actor>("Wolf", 3, std::string("grey"));
```

The translator writes a constructor table for each script class. The table has one entry for each constructor. A class without a constructor has one entry `()`. The `.hot` scripts and the C++ scripts get the same table. Each entry has these fields:

| Field | Meaning |
|---|---|
| `key` | The signature key: `typeid(hot::CtorSignature<P...>).name()`. `P` are the decayed parameter types. |
| `make` | A thunk `hot::Instance (*)(void** args)`. It casts each argument and calls the constructor. |
| `write_mask` | Bit `i` is set when parameter `i` is a `T&` or a `T&&` of a type that is not const. |

The decay removes the reference and the top-level `const` and `volatile` of each parameter. A type alias is the same type, so `IntFn f` and `int (*f)(int)` have the same key. The `ClassRecord` field `ctors` holds the table. A reload copies it, so the constructors of the current generation run. A removed class has no table.

The `ClassRecord` of a `.hot` class has more fields for script interfaces (see [Interfaces](inheritance.html#interfaces)):

| Field | Meaning |
|---|---|
| `iface_rows` | A table of `hot::IfaceRow` (`iface`, `method`, `sig`, `slot`). It lists all interface methods of the class, also the inherited ones. The table ends with a row with a null `iface`. Only the loader reads it. |
| `ifaces` | The names of the implemented interfaces. The list ends with `nullptr`. |
| `iface_slots` | The table from the session id of an interface method to a slot, or `hot::kNoIfaceSlot`. The loader builds it at each commit. |
| `iface_slot_count` | The size of `iface_slots`. An id outside the table means "not implemented". |

The call `hot::call_iface` uses these fields to call an interface method by name. A call site keeps the id of the method. The loader rebuilds the tables at each reload, so a call always uses the slots of the current generation.

The host API:

| Function | Meaning |
|---|---|
| `entry->construct(args...)` | Makes the decayed key of the argument types. It finds the constructor with the same key and calls it. Without arguments and without a `()` constructor, it calls `create`. |
| `entry->has_constructor<P...>()` | True when the class has a constructor with the decayed types `P`. |
| `entry->find_constructor(key)` | The table entry with the key, or null. |
| `loader.create_as<Native>(name, args...)` | Finds the class and calls `construct`. It gives the native base `Native*`. The caller owns the object. |
| `aot.create_as<Native>(name, args...)` | The same for an `AotRegistry`. `aot.find(name)` gives the `ClassEntry`. |
| `hot::ctor_key<P...>()` | The key of the parameter types `P`. |
| `hot::constructor_list(entry)` | The text of all constructors of the class, for example `(), (int), (Actor*)`. |

Rules of the match:

- The match is exact. The host does not convert an argument and does not use a default argument. For example, `construct(2.5)` does not call a constructor `(int)`.
- Compare keys with `std::strcmp`. Do not compare `type_info` addresses or `hash_code()`. They can be different in two modules.
- A `T&` parameter refers to the argument of the host. The constructor can change it.
- A `T&&` parameter gets a pointer to the argument. The constructor moves from it.
- A value parameter gets a copy of the argument.
- A const argument for a `T&` or a `T&&` parameter of a non-const type is an error.
- Two constructors with the same decayed types are not possible: the translator rejects them.

When no constructor matches, `construct` throws `std::invalid_argument`. The text names the class and lists its constructors, for example: `script class 'Made' has no constructor (double); its constructors: (), (int), (Actor*)`. `create_as` throws `std::runtime_error` when the class is not known.

### Options structs

`DefaultBackendOptions` (`include/hot/backend.h`):

| Field | Default | Meaning |
|---|---|---|
| `include_dirs` | empty | Added after the build-time include folders (`HOT_DEFAULT_INCLUDES`). |
| `extra_flags` | empty | Added after the build-time flags (`HOT_DEFAULT_CXXFLAGS`). |
| `fs` | empty | The file system of the host for the script imports and the headers. Empty: the disk only. It must be thread safe (see [Virtual file system](embedding-vfs.html#virtual-file-system)). |
| `kind` | `Auto` | The backend to make (see [Runtime](embedding-runtime.html)). |
| `std_version` | empty | The C++ standard of the scripts, for example `"c++20"`. A script with `async` needs C++20 (see [Async and await](async.html)). Empty: the standard of the host. |

`ClangFilesOptions` (`include/hot/clang_files_backend.h`):

| Field | Default | Meaning |
|---|---|---|
| `compiler` | `"clang++"` | The compiler command. |
| `include_dirs` | empty | For the script imports and for the compile. |
| `resource_dir` | empty | The Clang resource folder. Empty: the default of the compiler. |
| `std_version` | empty | For example `"c++17"`. Empty: the standard of the host (`hot::cxx_std_name()`). |
| `session` | empty | The translator cache. Empty: the backend makes its own. Share one to keep the cache between backends. |
| `extra_flags` | empty | Added to the compile command. |
| `work_dir` | `"hot_work"` | The `.cpp` and `.so` files go here. |
| `slots_file` | empty | Empty: `<work_dir>/slots.txt`. |
| `remove_work_dir_at_exit` | `false` | `true`: the destructor removes `work_dir`. Give a folder that no other backend uses. |
| `strict_borrowed` | `true` | `false`: a borrowed-storage finding is a warning. |
| `raw_diagnostics` | `false` | `true`: the error text is the raw compiler output. `false`: it uses script names. |
| `fs` | empty | The file system of the host for the script imports and the headers. Empty: the disk only. It must be thread safe (see [Virtual file system](embedding-vfs.html#virtual-file-system)). |

`JitOptions` (`include/hot/jit_backend.h`):

| Field | Default | Meaning |
|---|---|---|
| `compiler` | `"clang++"` | The driver name. An absolute path finds the toolchain best. |
| `include_dirs` | empty | For the script imports and for the compile. |
| `resource_dir` | empty | The Clang resource folder. |
| `std_version` | empty | As in `ClangFilesOptions`. |
| `session` | empty | The translator cache. |
| `extra_flags` | empty | Added to the compile command. |
| `pch_path` | empty | An optional precompiled header for `-include-pch`. |
| `strict_borrowed` | `true` | As in `ClangFilesOptions`. |
| `raw_diagnostics` | `false` | As in `ClangFilesOptions`. |
| `debug_flag` | `"-gline-tables-only"` | The debug flag. `"-g"` gives full debug info, but Clang leaks about 270 KB for each failed compile. Empty: no debug info. |
| `debugger_support` | `true` | Register the JIT code with the debugger (GDB JIT interface). |
| `fs` | empty | The file system of the host for the script imports and the headers. Empty: the disk only. It must be thread safe (see [Virtual file system](embedding-vfs.html#virtual-file-system)). |

`DefaultBackendOptions` sets only `include_dirs` and `extra_flags`. The default backend sets the compiler, the resource folder and (for `CLANG_FILES`) the work folder from the build.

## Weak pointers for pointer fields

`include/hot/runtime/checked_ptr.h`.

A pointer field `T*` of a native class is a `hot::CheckedPtr<T>`. It needs the trait `hot::weak_ref_traits<T>`. The runtime gives the trait for a class with `weak_from_this()`, such as a `std::enable_shared_from_this` class. A stub with `handle` or `weak_handle` gets a trait in the module header. A host smart pointer, such as the `WeakPtr` of rbfx, needs a specialization that the host writes in a header that the script includes:

```cpp
namespace hot
{
template <> struct weak_ref_traits<Urho3D::RefCounted>
    : custom_weak_ref<Urho3D::RefCounted, Urho3D::SharedPtr<Urho3D::RefCounted>, Urho3D::WeakPtr<Urho3D::RefCounted>>
{
    // Optional. Without it, a pointer field takes a handle only, not a raw pointer.
    static Urho3D::SharedPtr<Urho3D::RefCounted> from_raw(Urho3D::RefCounted* p)
    {
        return Urho3D::SharedPtr<Urho3D::RefCounted>(p);
    }
};
} // namespace hot
```

| Member | Meaning |
|---|---|
| `strong_type`, `weak_type` | The strong and the weak handle. The weak handle is default constructible. |
| `to_weak(strong)` | Makes the weak handle. |
| `lock(weak)` | Gives the strong handle, or a null handle when the object is gone. |
| `from_raw(T*)` | Optional. Gives the strong handle of a raw pointer. |

`hot::custom_weak_ref<T, Strong, Weak>` gives `to_weak` and `lock`. It reads the weak handle with `hot::lock`, which calls `lock()` or `Lock()`. `hot::shared_weak_ref<T>` is the same for `std::shared_ptr`.

Do not write a specialization for a class that already has a stub with `handle` in the script: the module header already has one. All modules that use the class must agree on one weak source. See "Custom handles" in [Types and handles](types-handles.html).

`CheckedPtr<T>::get()` gives the borrowed raw pointer, or null when the object is gone. `lock()` gives the strong handle.

## CMake

The page [CMake](cmake.html) describes the targets, the options and the functions `hot_add_host`, `hot_add_scripts` and `hot_script_host`. A complete project is the sample [aot](https://github.com/rokups/hotpp/tree/master/samples/aot).

## Format library for `${}`

The build chooses the format library for string interpolation. The page [CMake](cmake-options.html#the-format-library) has the rules and the table of `HOT_FMT_PROVIDER`.
