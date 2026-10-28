# S++

S++ is a hot-reloadable scripting language for C++. This is a prototype of its runtime and translator. This is step 3 (F1) with the backend interface.
The design is in `spp-design.md` in the project root.

## Layout

- `include/spp/` has the public headers: `runtime.h`, `module.h`, `backend.h`, `clang_files_backend.h`, `jit_backend.h` and `sppc_lib.h`. Use the form `#include "spp/<name>.h"`.
- `src/` has the library code. `src/runtime/` has the script runtime and the backends. `src/translator/` has the translator library `sppc_lib`. `src/tools/` has the tool `sppc`.
- `example/` has the example engine and the examples. `example/engine/` has the executable `minengine` (`Actor`, `World`, a small main program). `example/scripts/` has the example scripts. `tests/integration/fixtures/` has the hand-written generations. `example/bench_backends.cpp` is the benchmark.
- `tests/` has the GoogleTest cases and the shared headers.
- `cmake/` has `CPM.cmake` and `SppScripts.cmake` (`spp_add_scripts`).

## Build

The file `CMakePresets.json` has the presets. A build goes to `cmake-build/<preset>`.

    cmake --preset linux-clang-files
    cmake --build --preset linux-clang-files-dbg
    ctest --preset linux-clang-files-dbg

- `linux-clang-files` uses the backend `CLANG_FILES`. `linux-clang-jit` uses the backend `JIT`.
- Add `-asan` or `-tsan` to the preset name for a sanitizer build, for example `linux-clang-files-asan`.
- The build presets end with `-dbg` or `-rel`.
- The JIT backend also works with ThreadSanitizer. ORC and ThreadSanitizer do not conflict.

## Run

    cmake-build/linux-clang-files/Debug/minengine cmake-build/linux-clang-files/Debug/tests/integration/fixtures/gen1.so

The command loads `gen1.so` and makes one `Player`. It ticks the world 3 times. It exits with 0 on success.

The test `ReloadMigration` uses the path of a library that does not exist (`gen3_missing.so`). The load must fail.

## Translator

    cmake-build/linux-clang-files/Debug/sppc --emit-cpp example/scripts/player_v1.spp --gen 1 -o out.cpp -I include -I example/engine --slots player.slots

`sppc --emit-cpp` is a thin wrapper. It reads the files and calls `sppc::translate` from `sppc_lib` (`include/spp/sppc_lib.h`). The function takes strings: script text, script name for `#line`, generation, slot file content, include dirs and resource dir. It returns strings: C++ text, new slot file content and diagnostics. It does not write a file. The translator reads the script and the imported headers. It asks libclang (C API) for the virtual methods of the native base class. A `sppc::TranslatorSession` keeps the answer, see "Translator session". It writes the shell (one stub for each virtual), the state struct, the reflection table, the generation functions, the thunks and the record. It writes `#line` directives, so a Clang error points to the script line. It reports a missing pure virtual override and an override of a `final` method. A method with the name of a base virtual method and other parameter types is an error (it hides the virtual method).

The script has no brace initialization. Use parentheses: `T(args)` and `new T(args)`. A `{` in an expression gives an error. The script subset: `using`, `class N : Base`, fields `Type name;` or `Type name = init;` with `[alias("old")]`, methods `Ret name(T a) { ... }` (a method overrides a base virtual method when the name and the parameter types match; `void` means no return value), locals `Type name = e;`, `var name = e;`, `const Type name = e;` and `const var name = e;` (`var` gives `auto`), `this.`, `super.`, `null`, float literals, `Math.Min` as `Math::Min`, `if`, `while`, `for`, `return`, `throw`, `try`, `catch` and `finally`. A catch clause is `catch (Type e)` or `catch (...)`. A method can have a modifier: `override`, `virtual`, `new`, `public`, `private` or `protected`. A method that overrides a native virtual method needs `override`. The parser stores the access word and `virtual`. It does not enforce the access word, and `virtual` has no effect yet. A property is `Type N { get; set; }`, `{ get; }`, `{ get; private set; }` or `{ get { ... } set { ... } }`. The setter body reads the new value as `value`. A read or a write of `p.N` on another object of the class calls `get_N` or `set_N` of that object. A write gives the assigned value, so `a = p.N = 1` works. The rule uses the name only. If `p` is not an object of the class, the access throws `std::runtime_error`. The old forms `function`, `let`, `import`, `name: Type`, `-> Ret`, `for ... of` and `@was(...)` give a clear error. Native stubs tell the translator how a C++ type behaves. `[native, handle("std::shared_ptr<$T>")] class Weapon;` declares a handle type. `$T` is the class. `[native, value] class std.string;` declares a value type. A pattern stub covers many types: `std.vector<*>` or `physics.*`. A stub lives in the script or in a prelude file. Give a prelude file to `sppc --prelude file` or in `TranslateOptions::preludes`. A handle type in a field, a local, a return type and `foreach` uses the storage type. A handle parameter becomes `const H&`. `.` on a handle uses `spp::deref`. `std::shared_ptr` is built in. A custom handle needs the `spp::is_handle` trait. A type with no stub keeps its raw text. `new T(args)` needs parentheses. It gives `std::make_shared<T>(args)` for a shared_ptr handle, `H(new T(args))` for another handle and `T(args)` otherwise. See `example/scripts/handle_v1.spp` to `handle_v3.spp`. A reload that changes a field type is accepted (design 4.7): the `convert_<field>` hook converts the value, else the field gets its default value and the runtime logs a warning. See "Reload hooks". A handle stub on a native base class applies to all native subclasses. The nearest base wins. An exact stub on a subclass wins. A base stub beats a pattern stub. Only `handle` and `intrusive` are inherited. `value` is not inherited. The class hierarchy comes from libclang. A class in a system header is not in the map. A nested class and a template are not in the map. `intrusive("AddRef", "Release")` implies a handle: `[native, handle("$T*"), intrusive("AddRef", "Release")] class Object;`. If `handle` is given, it must be `$T*`. `intrusive` does not combine with `value`. The storage type is `spp::Intrusive<T, &Owner::AddRef, &Owner::Release>`. Owner is the class with the stub. A new object starts with zero references (the boost `intrusive_ptr` convention). The wrapper adds a reference when it is made from `T*`. The Add and Release methods must be non-overloaded, non-template members. A missing method gives a clang error that names it. A reload copies the field (add) and destroys the old block (release), so the count stays correct. See `example/scripts/intrusive_v1.spp`, `intrusive_v2.spp` and `example/engine/Object.h`. A script class is a handle. `class Name : Base { }` uses the handle of the native base. `class Name { }` has no base and is also a handle. `[value] class Name { }` is a struct inside the state block with copy semantics. A reload migrates it recursively. See `example/scripts/kinds_v1.spp` and `kinds_v2.spp`. Deferred to slice 2: member access on a script handle, the checks of `ref` against design 7.10.6, the checks of design 7.10.7, and the analysis of a parameter that a method stores as a copy. Native interface bases and their conflict checks are done (S8b, see "Native interface bases"). A comparison of an `Intrusive` with a raw `T*` is untested. Weak spot: a non-string attribute argument can give a spurious second error on a stub. A call argument `ref e` or `out e` becomes `spp::ref_arg(e)`. It converts to `T&` and to `T*`. The prefix `&e` gives the error "use 'ref' instead of '&'". The translator does not check `ref` against the not-storable rule yet. The arrow lambda `() => { ... }`, `(float x) => { ... }` and `(x) => { ... }` works in a method body. A parameter is `Type name`. A method parameter needs a type. A lambda parameter without a type is `auto`. The lambda body is a block. See "Closures and unload".

Builtin type names are fixed-width. The parser changes each alias to one canonical C++ name, so `ulong` and `uint64` give the same text in the emitted C++, in the override key and in the reload type key. `int` becomes `int32_t`. The table is in `src/translator/builtin_types.h` and in `spp-design.md` (section 7.1.1). The names: `bool`, `sbyte`/`int8`, `byte`/`uint8`, `short`/`int16`, `ushort`/`uint16`, `int`/`int32`, `uint`/`uint32`, `long`/`int64`, `ulong`/`uint64`, `nint`/`intptr`, `nuint`/`uintptr`, `float`, `double`, `char` (8 bit) and `wchar`/`char16` (`char16_t`). The C++ multi-word spellings are errors with a hint: `unsigned int` gives "use 'uint'", and `long long` gives "use 'long'". A native name such as `int32_t` or `size_t` stays a normal native name.

Imports: `using "Actor.h";` becomes `#include "Actor.h"`. A standard header uses angle brackets: `using <vector>;` or `using "<vector>";`. Both forms become `#include <vector>`, in the generated file and in the libclang probe. A missing header gives an error with the script name and line. The generated file always includes `<cstddef>`, `<cstdint>`, `<cstdio>`, `<new>`, `<string>`, `<type_traits>`, `<typeinfo>`, `<utility>` and `spp/runtime.h`. Add `using` for any other standard header, for example `<algorithm>`. A script can call a non-inline function of the host, for example `Distance` in `example/engine/Util.cpp`. The host executable must export it (`ENABLE_EXPORTS`, `-rdynamic`).

A script uses `.` for a scope and for a member access, for a pointer and for a reference. `std.string` becomes `std::string`. `p.m` becomes `spp::deref(p).m`, and the C++ compiler picks the pointer or the reference form. The text `::` and `->` in a script is an error: "use '.' instead of '::'" and "use '.' instead of '->'". The `=>` of a lambda stays. The old `->` return type is removed: the return type stands before the name.

A loop over a container is `foreach (Type x in c) stmt`. The head can also be `foreach (var x in c)`, `foreach (const Type x in c)` or `foreach (const var x in c)`. The old form `for (x of c)` is an error: "'for ... of' is removed; write 'foreach (Type x in c)'". These words cannot be names of variables: `var`, `foreach`, `in`, `using`, `override`, `virtual`, `new`, `public`, `private`, `protected`, `get`, `set`, `finally` and `void`. A member name after a `.` can be any of them, for example `p.get()`.

An initializer is optional. With no initializer, a field, a property or a local has the default value: zero, `false`, null or the default constructor. `var` and `const` need an initializer.

Limits of the `.` access. The translator writes `a.b` as `spp::deref(a).b`. This works for a pointer and for a reference only. A type with `operator->` has no member access through `.`: a smart pointer, an iterator and `std::optional`. For a field `std.unique_ptr<Actor> p;`, `this.p.x` gives a Clang error "no member named 'x'". Call `.get()` or `.operator*()` first, for example `this.p.get().x`. A native global or a static data member, for example `Vec3.Zero.x`, becomes `Vec3::Zero.x`. The scope chain does not go through `spp::deref`. Copy the value to a typed local first. An array has no `.` access (the `deref` overload for an array is deleted).

The slot file (`--slots`) keeps the slot order of the script methods stable between generations. A generation can add a method, but it cannot remove one.

### Module declarations (S8a)

A script can declare enums, structs and module variables at file level (`DECISIONS.md` item 11). See `example/scripts/module_v1.spp` and `module_v2.spp`.

- `enum Name [: type] { A, B = 5, C }` becomes `enum class Name : T`. The default type is `int`. `Name.A` becomes `Name::A`. A reload migrates an enum field as an integer.
- `struct Name { ... }` is the same as `[value] class Name { ... }`.
- A module variable is `[alias("old")] Type name [= init];` at file level. Every class of the script reads it by its bare name. The translator puts the variables in a synthetic `[value]` class `SppModule`. The loader owns one module block. A reload migrates it like the fields of a class.
- Limits: a module variable is a native type, an enum or a `[value]` class. An initializer cannot use another module variable. A convert hook for a module variable exists only in the runtime (`FieldInfo.convert`).

### Native interface bases (S8b)

A script class can have more than one native base: `class Boss : Actor, IDamageable, ITagged`. The first base is the primary base. The other bases are interface bases (design 4.4). See `example/scripts/boss_v1.spp`, `boss_v2.spp`, `boss_rebase.spp` and `example/engine/Damageable.h`.

- The shell derives from all bases: `struct Boss_Shell final : Actor, IDamageable, ITagged, spp::ShellBase`. The constructor arguments go to the primary base. An interface base needs a default constructor.
- libclang reads the virtuals of each base. Each virtual gets one slot. The slots of the primary base come first, then the slots of each interface base in the order of the list. A name in two bases gets a suffix of its parameter types, for example `SLOT_Size_i`.
- `override` works for a virtual of any base. A virtual without an override calls the implementation of its base, for example `IDamageable::Armor`. `super.Armor()` calls the interface implementation when the primary base has no virtual with that name.
- The record keeps the list of the bases in `base_name` (`"Actor, IDamageable, ITagged"`). The shell hash uses the list. A reload that adds or removes a base is rejected (design 5.3: RECONSTRUCT): "native interface bases changed".
- Errors: the same signature in two bases; an override that matches `F()` of one base and `F() const` of another base; an override of a `final` virtual; a `final` interface class; a missing pure virtual override; a base named two times; a script class as interface base; interface bases on a class with a script parent; an interface base without a default constructor.
- Limits: a child of a script class cannot add interface bases. It inherits the interface bases of the root of its family.

### Translator session

libclang needs about 400 ms to parse the engine headers. `sppc::TranslatorSession` (`include/spp/sppc_lib.h`) keeps the result: the virtual methods, the destructor and the constructors of each native base class. `sppc::translate(options, session)` uses it. `sppc::translate(options)` makes a temporary session, so it parses each time.

- The key is the include flags, the current directory and the set of imports. A script that names a base class that the entry does not have causes one new parse. The next calls for both bases hit.
- The entry keeps the path, the change time (ns) and the size of each file that the parse included (`clang_getInclusions`, with the system headers). Each call checks them with `stat`. If one differs, the session parses again. The check costs about 1 ms.
- The cache does not see a new header that comes earlier in the include path, or a change that keeps the same time and size. Use `TranslatorSession::clear()` for these cases.
- Calls that use one session are thread safe. They wait for each other during a parse.
- `ClangFilesBackend` and `JitBackend` hold a session. `ClangFilesOptions::session` and `JitOptions::session` share one session between backends. `sppc` and `sppc` do one translate for each run, so they do not gain.

## Backends

A backend turns script source into a loadable `Module`. A `Module` has the `script_register` function and an owner of the code memory (for a `.so`: a `dlopen` handle). The `Loader` takes a `Module` in `load` and in `request_reload`. The class `Backend` (`include/spp/backend.h`) has the method `load(source, generation)`. It returns a `Module` or an error text with the compiler diagnostics.

The CMake cache option `SPP_BACKEND` selects the backend at build time. `make_default_backend()` returns it.

- `CLANG_FILES` (default). `ClangFilesBackend` calls `sppc::translate` in the process, writes the `.cpp` in a work directory, runs an external `clang++` to make a `.so`, and loads it. The options are the compiler command, the include dirs, the resource dir, the extra flags and the work directory.
- `JIT`. `JitBackend` calls `sppc::translate` in the process, compiles the C++ text in memory with the Clang library and loads the code with LLVM ORC. See the section "JIT backend".

Example: `cmake --preset linux-clang-files`.

## JIT backend

`cmake --preset linux-clang-jit`. LLVM and Clang (23.1.3 here) must be installed. CMake uses `find_package(LLVM)` and `find_package(Clang)` and links `clang-cpp` and `LLVM`. CPM does not fetch them.

How it works (`src/runtime/jit_backend.cpp`):

1. `sppc::translate` makes the C++ text. The slot file text stays in memory in the backend object.
2. `clang::createInvocation` builds the cc1 command through the driver (so the libstdc++ include paths are right). The flags are `-std=c++20 -O1 -fPIC -gline-tables-only`, the include dirs, the resource dir and the extra flags. The input file exists only in an `InMemoryFileSystem` that overlays the real file system. `#line` makes the diagnostics name the script.
3. `EmitLLVMOnlyAction` gives an `llvm::Module`. The diagnostics go to a string.
4. One `LLJIT` (JITLink) lives for the session. Each `load` makes a new `JITDylib` and a `ResourceTracker`, adds the module, runs the static initializers (`LLJIT::initialize`) and looks up `script_register`. The Main JITDylib has a `DynamicLibrarySearchGenerator` for the process. The executable needs `ENABLE_EXPORTS` (`-rdynamic`), and it must link the runtime objects that the script code uses.
5. The `keep_alive` of the `Module` owns the generation. When the last copy goes, it runs `LLJIT::deinitialize`, `ResourceTracker::remove` and `removeJITDylib`. It also holds the `LLJIT`, so a `Module` can outlive the backend. As in the design, ORC does not track calls in flight. The safety rules are the same as for `dlclose`.

No file is written and no process starts. `tests/integration/backends/jit_backend_tests.cpp` checks this: it sets the current directory and `TMPDIR` to a new empty directory, loads two generations and a bad script, and asserts that the directory is empty and that no child process ran. `strace -f` on the same test shows one `execve` (the test itself), only thread `clone3` calls and no file opened for writing. The same file tests exceptions (a `throw` and a `catch` in JIT code, and a `throw` that goes through JIT frames to the host), `-include-pch`, and the life of a generation.

Facts that differ from the first design:

- `-g` leaks. After a failed compile, Clang does not free the temporary debug metadata (about 270 KB for `bad.spp`, found with LeakSanitizer). `-gline-tables-only` does not leak, and it costs the same time. `JitOptions::debug_flag` sets the flag. LLJIT does not register a debugger plugin, so full debug info has no use now.
- `CompilerInstance::ExecuteAction` prints "N errors generated." to stderr when `ShowCarets` is on. The backend uses its own copy of the diagnostic options for the printer, so the text goes only to the result.
- `CompilerInvocation` keeps pointers into the argument strings. The strings must live as long as the compile.
- `LLVMConfig.cmake` probes C headers, so CMake calls `enable_language(C)` for the JIT build.
- The global constructors of a script (the class record) are run by `LLJIT::initialize`. Without it, `Player_record` has no value.

### Measurements

Machine: 32 threads, Linux 7.2, clang 23.1.3, `-O0`/`-O1` as noted, the page cache is warm. Tool: `cmake-build/<preset>/Debug/bench_backends` (`example/bench_backends.cpp`, `std::chrono`). Times in ms for one `load` (translate + compile + load) of `player_v1.spp` (v1) and `player_v2.spp` (v2). "Cold" is the first load in a new process (a new session). "Warm" is the median of 5 later loads in the same process. Warm backends share one translator session, except in the rows "no cache" (`--no-cache`). Cold v1 always parses the headers. Cold v2 and all warm loads use the cache, except v1 in the rows "no cache".

| Configuration | v1 cold | v2 cold | v1 warm | v2 warm |
|---|---|---|---|---|
| JIT, `-O1` (default), cache | 1306 | 806 | 812 | 808 |
| JIT, `-O1`, no cache | 1295 | 812 | 1220 | 809 |
| JIT, `-O0`, cache | 1283 | 781 | 786 | 789 |
| JIT, `-O1`, `-include-pch`, cache | 622 | 166 | 170 | 166 |
| JIT, `-O1`, `-include-pch`, no cache | 637 | 166 | 586 | 169 |
| CLANG_FILES, `-O0 -g` (default), cache | 1370 | 908 | 913 | 907 |
| CLANG_FILES, `-O1`, cache | 1394 | 941 | 957 | 947 |
| CLANG_FILES, `-O1`, no cache | 1395 | 940 | 1356 | 940 |

CLANG_FILES with a PCH is not measured: the bench tool makes the PCH only in the JIT backend.

Before the cache, the translate time was 411 ms in each load. The split of one translate (v2, `std::chrono` in `translate_impl`, measured before the change) was: libclang parse of the imports 406 ms, script parser 0.4 ms, visit of the base class 0.5 ms, text emit 2.7 ms. Only the libclang parse is large.

Translate alone (the bench prints it): 406 ms with a new session (miss), 1.3 ms with the same session (hit). The target was 50 ms.

Phases of one JIT load (v2, warm, cache): translate 1.4, compile 782, ORC (code generation, link, initializers, lookup) 19. With `-include-pch`: translate 1.4, compile 146, ORC 18. A new session parses the headers again, so v1 warm in the rows "no cache" costs 411 ms more.

- JIT against CLANG_FILES with the same `-O1`: about 140 ms less (15%). The saving is the process start and the link step. The compile itself is the main cost in both.
- `-include-pch` works with the in-memory compile. `JitBackend::make_pch(header, out)` makes the PCH in the process (886 ms once, 10 MB). The header must start with the same includes as the generated file. It saves 636 ms of 782 ms in the compile phase (81%) and 642 ms of 808 ms in a load (79%, with the translator cache). The PCH keeps the path and the time stamp of the header and needs the same flags (for example `-fsanitize`).
- Size: `backend_tests` is 1.29 MB (CLANG_FILES) and 1.69 MB (JIT), so +0.4 MB. Both builds already need `libclang.so` (45 MB) and `libLLVM.so` (173 MB) for the translator. The JIT build also needs `libclang-cpp.so` (78 MB). Peak RSS of the bench process: 100 MB (CLANG_FILES), 299 MB (JIT), 246 MB (JIT with PCH).

## Precompiled scripts

The tool `sppc` compiles scripts ahead of time. This is not a backend. Any build can load the result. Call `spp::module_from_library(path)` and give the `Module` to the `Loader`. This call needs no backend object.

    sppc player_v1.spp --gen 1 -I include -I example/engine --out-dir out/
    sppc player_v2.spp --gen 2 -I include -I example/engine --out-dir out/

The tool `sppc` has two modes. The default mode builds a `.so`. The mode `--emit-cpp` only translates (see "Translator").

Command line of the build mode:

    sppc <script>... --gen N (--out-dir DIR | -o x.so) [-I dir]... [--resource-dir dir]
         [--cxx command] [--cxxflag flag]... [--slots file] [--no-strict-borrowed]

Command line of the translate mode. Without `-o`, the tool prints the C++ to the standard output:

    sppc --emit-cpp <script> --gen N [-o out.cpp] [-I dir]... [--prelude file]...
         [--slots file] [--resource-dir dir] [--no-strict-borrowed]

- `--raw-diagnostics` prints the compiler text as it is. By default `sppc` rewrites it (see "Diagnostics and debugging").
- `--no-strict-borrowed` makes the borrowed-storage findings (a stored `T*`, `T&` or `ref`) warnings. By default they are errors.
- The order of the classes in a script does not matter. A cycle of classes is an error.
- `<script>...` is one or more script files. Each script gives one library.
- `--gen N` is the generation number (1 or more).
- `-o x.so` builds one script into the library `x.so`. The C++ file goes to the directory of `x.so`. Use `-o` or `--out-dir`, not both.
- `--out-dir DIR` is the output directory. For a script `player_v1.spp`, `sppc` writes `DIR/player_v1.g1.cpp` and `DIR/player_v1.g1.so`.
- `-I dir` is an include dir. The translator and the compiler use it.
- `--resource-dir dir` is the Clang resource dir. The default is the one that the build found.
- `--cxx command` is the compiler command. The default is `clang++`.
- `--cxxflag flag` adds one flag to the compile command. Repeat it for more flags.
- `--slots file` is the slot file. The default is `DIR/slots.txt`. It keeps the slot order of the script methods between generations. Build the generations of one class in order.

The exit code is 0 for success, 1 for an error and 2 for a wrong command line. The tool prints the compiler errors at the script line. It does not need CMake.

The CMake function `spp_add_scripts` (`cmake/SppScripts.cmake`) only runs `sppc`:

    spp_add_scripts(TARGET pre_gen1 GEN 1 SCRIPTS example/scripts/player_v1.spp
                    INCLUDES include example/engine FLAGS ${flags} DEPENDS ${headers})
    spp_add_scripts(TARGET pre_gen2 GEN 2 SCRIPTS example/scripts/player_v2.spp AFTER pre_gen1 ...)

`TARGET` is a custom target that makes the libraries in `OUT_DIR` (default `${CMAKE_BINARY_DIR}/spp_scripts`). `AFTER` orders a generation after the earlier one.

## Shipping mode

A shipping build has no loader, no reload, no JIT and no Clang (design M7). The generated C++ of all scripts is part of the game binary. The objects are plain C++ objects with the same shell classes. The class records, the slot tables and the factories are constant data of the binary.

Command line (S12). The tool translates generation 1 of each script. It does not compile:

    sppc --ship <script>... --out-dir DIR [-I dir]... [--prelude file]... [--no-strict-borrowed]
         [--format=std|fmt] [--resource-dir dir]

- For each script `sppc` writes `DIR/<name>.ship.cpp`. The register function has the name `spp_script_register_<name>`, so many scripts link into one program.
- `sppc` also writes `DIR/spp_shipped_registry.cpp`. It defines the constant table `spp::shipped_table` (`include/spp/shipped.h`).
- The script names must be different. The class names must be different over all the scripts of a program. A shipped script has no slot file and no later generation.

The CMake function `spp_add_shipped_scripts` (`cmake/SppScripts.cmake`) runs `sppc --ship` and builds a static library:

    spp_add_shipped_scripts(TARGET game_scripts SCRIPTS player.spp boss.spp
                            INCLUDES include example/engine [PRELUDES file...] [DEPENDS file...])
    target_link_libraries(game PRIVATE game_scripts)

The library links `spp_ship`. This target holds the core of the runtime (`threads.cpp`, `registry.cpp`) and the shipped registry (`shipped.cpp`). The loader, the migration and `dlopen` are in the other target `spp_runtime`. Do not link both targets in one program.

At start-up the program makes the registry. The registry calls the register function of each script, makes the blocks of the module variables and sets `spp::active_generation` to 1:

    spp::ShippedRegistry registry(spp::shipped_table);
    Actor* boss = registry.create_as<Actor>("Boss");   // the factory: the constructor with default arguments
    world.Add(boss);                                    // the engine ticks it as before

`new` of a script class, script virtuals, properties, `is`/`as` and constructors work as under the loader, because the code is the same. The generation guard stays in the code and costs one compare. A shipped program never runs `onReload`.

The example is `example/ship/main.cpp` (target `ship_demo`) with the script `example/scripts/ship_demo.spp`. The tests are in `tests/integration/ship/`. They run the same steps with the loader and with the shipped program and compare the text. They check with `nm` that `ship_demo` has no loader and no Clang symbol. A compile error in the shipped C++ shows the generated file, not the script line.

## Test

CMake uses CPM. It takes GoogleTest from the system, and fetches it only when it is missing. libclang must come from the system. The JIT build also needs the LLVM and Clang development packages.


    cmake --build --preset linux-clang-files-dbg && ctest --preset linux-clang-files-dbg

All tests are GoogleTest cases. CMake registers them with `gtest_discover_tests`.

- `HandwrittenGeneration` (`tests/integration/reload/handwritten_generation_tests.cpp`) loads `gen1.so` and makes one `Player`. It ticks the world 3 times and calls both `TakeDamage` overloads. It checks the type name, the position and the field `hp` by reflection.
- `ReloadMigration` (`tests/integration/reload/reload_migration_tests.cpp`) binds a delegate to `heal` through a thunk. It reloads gen 2 at the frame boundary and checks the address, the migrated state, the new `Tick` and the delegate. It then tries gen 4 and a missing library. Both must be rejected, and gen 2 must keep running.
- `BackendTests` (`tests/integration/reload/`) use the backend that `SPP_BACKEND` selected. They run the F1 reload scenario (`tests/support/f1_scenario.h`): generation 1 then generation 2, the same address, `hp` becomes `health`, `shield` is 25 and a delegate calls the new `heal`. A bad script (`example/scripts/bad.spp`) must give an error that names `bad.spp` and the line. The runtime must keep running after the error.
- `SessionCache` (`tests/unit/translator/session/session_cache_tests.cpp`) checks the cache: a hit gives the same output as a miss, a changed header (also a nested one) invalidates it, a second base class works in the same session, and the second translate takes less than 150 ms (the real value is about 1.3 ms).
- `JitTests` (`tests/integration/backends/jit_backend_tests.cpp`, only in a JIT build) check that no file appears and no child process runs. They also check exceptions in JIT code, a precompiled header and the life of a generation.
- `Closure`, `GenerationUnload` (`tests/integration/closures/`) test escaped closures and the unload of generations with `example/scripts/timer_v1.spp` and `timer_v2.spp`. See "Closures and unload".
- `LambdaTranslation` (`tests/unit/translator/translate/lambda_translation_tests.cpp`) checks the output for an arrow lambda.
- The other translator tests are in `tests/unit/translator/`. Each folder is one test program: `parser`, `lowering`, `emit`, `types`, `diagnostics`, `session` and `translate`. The golden-output tests are in `tests/golden/`. The expected files are in `tests/golden/expected/`. Set `SPP_UPDATE_GOLDEN` to write them again.
- `PrecompiledTests` load the libraries that `spp_add_scripts` made with `sppc`, through `module_from_library`. They run the same F1 scenario. They run for each `SPP_BACKEND` value. One test runs `sppc` on `bad.spp` and checks the exit code and the error line.

## ImGui example

`imgui_app` (`example/imgui/`) draws a Dear ImGui window. A script class draws the user interface. The script includes the real `imgui.h` (`using "imgui.h";`) and calls the ImGui API as it is: `ImGui.Begin("x")`, `ImGui.Text("n: %d", this.n)`, `ImGui.Checkbox("c", ref this.on)`. A pointer parameter takes the address of a field. There is no wrapper layer.

Build. The target exists only when CMake finds SDL3 (`find_package(SDL3)`). Without SDL3 the configure step prints a STATUS message and the rest of the build is the same. CPM fetches Dear ImGui (v1.92.9b) and builds the static library `imgui_sdl3` with the SDL3 and SDL_Renderer backends. The executable links it as a whole archive, so a script can call every ImGui function.

    cmake --preset linux-clang-files
    cmake --build --preset linux-clang-files-dbg --target imgui_app

Run. The default script is `example/imgui/demo.spp`.

    cmake-build/linux-clang-files/Debug/imgui_app [script]

Hot reload. The host checks the modification time of the script file at each frame. When it changes, the host compiles the script as a new generation and starts a reload. The commit happens after `Frame()`, at the frame boundary. The object keeps its field values. If the compile or the reload fails, the window "Script error" shows the text and the old generation keeps running. If `Frame()` throws, the host drops the object and shows the message. The next good reload makes a new object. The host writes `reloaded gen N` and `reload failed: ...` to stderr.

The base class is `UiApp` (`example/imgui/UiApp.h`, virtual `Frame()`). Gap: a native array field such as `float rgb[3];` gives "expected a member name", so `ImGui.ColorEdit3("c", this.rgb)` does not work. Use `ImVec4 col` and `ref this.col.x`, or three float fields.

## Reload

`Loader::request_reload` validates a `Module` with no side effect: `script_register`, base class, shell layout hash, slot count. `Loader::commit_pending` runs at the frame boundary (the `World` hook). It allocates all new state blocks, migrates, swaps the table and layout, sets `active_generation` and frees the old blocks.

## Closures and unload

An arrow lambda becomes `spp::make_closure(GEN, "Class::fn lambda", [=](params) { ... })`. It captures by value. `this` is the pointer `self`. The body reads the state pointer again, so a lambda never keeps a pointer to a state block. The result is a `spp::Closure<Sig>`. It holds one pin of its generation (`spp::pin`, `unpin`, `pin_count`; the counts are atomic). Each call checks the generation. When the generation is not live, the call logs a stale call, counts it (`spp::stale_call_count()`) and returns a default value. The body does not run. A `Closure` converts to `std::function` and to an engine delegate that has `Bind(std::function<Sig>)`.

`World::AddTimer(seconds, fn)` stores a one-shot callable. `World::Tick` fires it before the frame boundary hook and then destroys it. `ClearTimers()` drops all timers.

The `Loader` owns all modules. A reload retires the old generation. `Loader::frame_boundary()` (call it in the hook, after `commit_pending`) unloads a retired generation when all of these are true: the pin count is zero, a frame boundary passed after the commit, and `code_refs` is zero. The unload is `dlclose` (CLANG_FILES) or the removal of the ORC resource tracker (JIT). `Loader::stats()` gives the active generation, each generation with its state (active, retired, unloaded), its pins and its `code_refs`, and the number of live objects.

Result of R3: the stale call is logged once and the old body does not run. No ASan error. Result of R6: generation 2 of the chain 1, 2, 3 unloads after its closure is dropped, and later ticks are clean. Generation 1 unloads too (S10). A core module holds the class record, the factory, the thunks and the vtables. The core is a second instance of the same generation code, and it never unloads. Destroy the last copy of a closure on the main thread. The destructor code is in the module.

## Reload hooks

A script class can define two kinds of hook (design 4.7). A hook is not a script function. It gets no slot and no thunk.

- `void onReload(OldState old)` runs once for each live object after the migration of all objects. It runs in the new generation. `old.has("name")` is true when the old generation had a field with this name. This includes a field that the migration moved, so `old.has("hp")` works for `[alias("hp")] float health`. `old.has<T>("name")` also checks the type. `old.get<T>("name")` gives a `const T&` and throws `std::runtime_error` when the field is absent or has another type. A throw out of the hook is caught and logged (`[script] ERROR: onReload of Hero threw: ...`). The object stays valid and the reload completes.
- `T convert_<field>(Old old)` converts a field after a type change. `<field>` is the new field name. `Old` is the old type of the field. `T` is the new type of the field. The runtime calls it when the old field type is `Old`. Else it uses the default value and logs a warning. The function has no `this` and no access to the other fields, because the new block is not complete.
- A rename without `[alias]` is a remove plus an add. If the removed field and a new field have the same type, the runtime logs a warning that names the `[alias]` fix. The value is lost.
- A native move constructor that throws during the migration is fatal (design 6.6). The runtime calls `spp::request_restart`. Without a handler (`spp::set_restart_handler`) it aborts. With a handler that returns, the commit returns false and the old generation stays.
- The reload still rejects a change of the native base class and a change of the shell layout.

See `example/scripts/hooks_v1.spp` to `hooks_v3.spp`, `tests/unit/runtime/migration_tests.cpp` and `tests/integration/reload/hook_tests.cpp`.

## Diagnostics and debugging

Diagnostics (S11, design 4.2 and 4.12):

- `sppc::rewrite_diagnostic(text, scripts, generated)` is a pure function in the translator library. It changes the Clang text to script names (`Player_Shell` to `Player`, `Player__heal` to `Player.heal`, `int32_t` to `int`). It does not count the hidden `self` argument. It gives a plain message for the common mistakes (the table is in `src/translator/diag_rewrite.cpp`). It shows the script line in place of the generated line.
- Both backends and `sppc` use it. `raw_diagnostics` in the backend options and `sppc --raw-diagnostics` keep the compiler text.
- The column of a script location is always 1. `#line` does not map the column.
- The backends compile with `-Werror=return-type`. A missing `return` is an error.
- The translator errors do not pass through the rewriter.
- The golden cases are in `tests/golden/diagnostics/` (21 mistakes). The test is `DiagnosticMistakes`.

Debug information:

- The generated C++ has `#line` directives. The JIT and `sppc` use `-gline-tables-only`, so the DWARF line table names the `.spp` file. The tests `DebugInfo.*` check this with `llvm-dwarfdump`.
- The JIT backend registers each object with the debugger through the GDB JIT interface (`llvm::orc::enableDebuggerSupport`). `JitOptions::debugger_support` turns it off. `debugger_support()` and `debugger_error()` tell the result. The test `JitTests.DebuggerGetsObjectWithScriptLines` reads `__jit_debug_descriptor`. LLVM 23 has no `createJITLoaderGDBRegistrar`.
- `tools/lldb/spp_formatters.py` and `tools/lldb/spp.lldbinit` show state blocks, shells and handles by script name. The command `spp-bt` prints the script frames. `tools/natvis/spp.natvis` does the same for Visual Studio.
- The pure Python helpers have a test (`PythonLldbFormatters`, `python3 -I`). lldb itself, a real debugger session and Visual Studio were NOT run. The F3 check (breakpoint in `heal`, step, inspect `this.health`) stays open.

## Language server

`spp-ls` (S14b, design 10.1, M12 to M14) is a stdio LSP 3.17 server. The code is in `src/ls/`. The extension in `editors/vscode/` starts it.

How it works:

- The server owns the text of each open `.spp` document. On each change it runs `sppc::analyze()`. It gets the parse errors, the facts (classes, members), the generated C++ and the source map.
- It starts a stock `clangd` as a child process. It gives the generated C++ to clangd as the virtual file `<script>.spp.cpp`. The compile command comes from `spp::compile_command`, through `workspace/didChangeConfiguration`.
- It maps each position between the script and the generated C++ with the source map. A line without a segment uses the `#line` directives.
- `sppc::rewrite_generated_names` and `sppc::rewrite_clang_message` change generated names to script names in hover text, diagnostics and completion labels.
- JSON comes from nlohmann_json (CPM).

Features:

- Diagnostics: the parse errors of analyze, and the clangd errors at the script position.
- Completion: `this.` lists the script members and the native members of the base. The server puts the placeholder `__spp_complete` in place of the word at the cursor, translates again and asks clangd at the placeholder.
- Hover, go-to-definition and references. A script symbol goes to the script. A native symbol goes to the native header.
- Document symbols come from analyze. Semantic tokens come from a small lexer and the facts.
- Rename changes the script symbols in the document. Rename of a native symbol gives the error `cannot rename a native symbol: X`.
- Workspace symbols come from the clangd index. Give `--compile-commands-dir` with the output dir of `sppc --compile-commands`. Then clangd runs its background index on the generated files. A symbol of a generated file maps to its script member. The helpers of the translator (thunks, slot functions) are not shown.

The `#line` check: clangd reports the real line of the generated file, not the `#line` line. So the generated C++ keeps its `#line` directives. No translator option is necessary.

From C++ to the script:

- In a `.cpp` file from `sppc --compile-commands`, the `#line` directives name the `.spp` file. A debugger and the compiler use them. clangd does not use them for navigation.
- The custom request `spp/toScript` (params: `textDocument`, `position`) gives the script location of a position in a generated file. It uses the source map for an open script, and the `#line` directives for a file on disk.
- The test `LanguageServer.GeneratedFileOnDiskMapsBackToTheScript` checks it.

Limits:

- Rename changes only the open document. It does not change other `.spp` files.
- After a translation error, clangd keeps the last good generated C++. Its positions can be old until the next good translation.
- The column of a `#line` fallback is 0.

The tests are in `tests/integration/ls/` (`LanguageServer.*`). They run the server on a thread with an LSP client over pipes. The clangd tests are skipped when `clangd` is missing. Set `SPP_LS_CLANGD_LOG=file` to get the clangd log of a test.

## Not done yet

Unload of the first generation of a class, string interpolation, script inheritance and constructors in scripts.

## Threads and the safe point

The tests are in `tests/integration/threading/parallel_tick_tests.cpp` (suite `ParallelTick`).

`World::TickParallel(dt, threads)` ticks the actors on worker threads. The workers start once. They stop when the world ends. Each worker ticks one chunk of the actor list. The main thread waits at a barrier. After all workers reach the barrier, the main thread runs the frame boundary hook. The workers stay idle until the hook ends.

The reload commit runs only in this hook. At that time no thread is in script code. The barrier also orders the memory, so the workers see the result of the commit.

A compile thread can call `Loader::request_reload` and `Loader::reload_pending` at any time. These functions take the mutex of the loader. They only validate. They change no live class and no live object. `commit_pending` takes the same mutex.

The debug check is the counter `spp::in_script_tick()`. The engine wraps each actor tick in a `spp::ScriptTickScope`. `commit_pending` asserts that the counter is zero. It also counts a violation in `spp::unsafe_commit_count()`. The check exists in debug builds only. With `NDEBUG`, `spp::kTickChecks` is false, `ScriptTickScope` does nothing and the commit has no check.

A class that a new generation drops is removed (design 6.1). Its record stays. Its live objects keep their state. Each slot points to `spp::removed_class_handler()`, which logs the first call, counts all calls and returns zero. A floating point result of such a call is not defined. The class can come back in a later generation. `Loader::stats()` and `Loader::stats_text()` (the `script_stats` text) list the generations, the pins, the live objects, the classes with their slots and the removed classes.

`minengine script.spp [Class]` compiles the script, ticks at 60 Hz and reloads when the file changes (`FileWatcher`: inotify on Linux, polling else). Commands on stdin: `reload`, `stats`, `quit`.

The warnings of a reload use readable names: C++ type names (`int`, `std::string`) and class names (`Hero`, not `HeroState_g2`).

Rules for threads:

- A worker reads class records and state blocks. It writes the state of its own actors only.
- A worker does not create or destroy script objects. It does not call the loader.
- The main thread calls `load`, `commit_pending`, `frame_boundary` and `stats`.
- Any thread can pin or unpin a generation.

Test results: `R5_CommitOnlyAtBarrier` runs 20 seeds with 1000 actors and 2 workers. `R5_RejectedReloadFromOtherThread` runs 5 seeds. Both tests pass in the normal, ASan and TSan builds. They also pass with `--gtest_repeat=20` in the normal and TSan builds. TSan reports no race. When the lock in `request_reload` is removed, TSan reports a race. So the test can find this kind of error.
