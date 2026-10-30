# S++

S++ is a hot-reloadable scripting language for C++. This is a prototype of its runtime, translator, tools and language server.
The design is in `spp-design.md` in the project root. See "Status" for the milestones.

## Status

Each row is a milestone of `spp-design.md` section 10. "Not testable here" means that the work needs a part that this machine does not have. `DECISIONS.md` has the decisions and the reasons. `TASKS.md` has the slices. `HANDOFF.md` has the state and the limits.

| Milestone | Status | Note |
|---|---|---|
| M0 Spikes | Partial | R0 and P1 are done with the test host `tests/engine`. The spikes for risk 2 and 3 need the real engine: not testable here. |
| M1 Runtime spine | Done | R1 to R6 pass, also under ASan and TSan. |
| M2 Translator v0 | Done | F1 passes, also for a native base that derives from a class template instance, a template over a template and a partial specialization (design risk 8). `class Foo : Tmpl<int>` works. |
| M3 Diagnostics and debugging | Partial | F2 passes. F3 passes with gdb. lldb, the lldb formatters in a live session and Visual Studio with natvis are not testable here. |
| M4 Engine integration | Not testable here | The test host `tests/engine` and the samples in `samples/` are the only hosts. A real engine is needed. |
| M5 Contract enforcement | Done | The rows of design 5.3 have tests in `tests/integration/reload/` and `tests/unit/translator/translate/`. |
| M6 Language v1 | Done | Inheritance, virtuals, `is`/`as`, lambdas, enums, structs, module variables, interpolation, constructors. Coroutines are not in version 1. |
| M7 AOT compilation | Done | `sppc --aot`. Generation 1 only. |
| M8 Unloading | Done | Core module. Generation 1 unloads. 1000 reloads with `SPP_STRESS=1`. |
| M9 Optional ORC backend | Partial | The JIT backend works on Linux. The debug-symbol resolver for Windows is not testable here. |
| M10 Source map and tolerant parse | Done | `TranslateResult::source_map`, `sppc::analyze`. |
| M11 VS Code extension | Done | Grammar snapshot test and package. A real VS Code session is not testable here. |
| M12 Language server stage 1 | Done | `spp-ls`, a server that uses a stock clangd as a child process (no fork). |
| M13 Language server stage 2 | Done | One test for each feature. |
| M14 Language server stage 3 | Done | Background index, `spp/toScript`, rename across the workspace, `sppc --compile-commands`. |

Not done, and not testable here: Windows and macOS, a real engine, a real lldb and Visual Studio session, the optional ORC debug-symbol resolver, and engine coroutines (version 2).

## Layout

- `include/spp/` has the public headers: `runtime.h`, `module.h`, `backend.h`, `clang_files_backend.h`, `jit_backend.h` and `sppc_lib.h`. Use the form `#include "spp/<name>.h"`.
- `src/` has the library code. `src/runtime/` has the script runtime and the backends. `src/translator/` has the translator library `sppc_lib`. `src/tools/` has the tool `sppc`.
- `tests/engine/` is the test host (`Actor`, `World`, `Harness`, `FileWatcher`). It is not a model for a user project: copy from `samples/`. `tests/scripts/` has the test scripts. `tests/integration/aot/` has the program `aot_demo`. `tests/integration/fixtures/` has the hand-written generations. `tools/bench/bench_backends.cpp` is the benchmark.
- `samples/` has seven self-contained projects for consumers of S++ (see "Samples"): six show one way to host S++ each, and `imgui` is a Dear ImGui demo. Each one shows the `add_subdirectory` pattern.
- `tests/` has the GoogleTest cases and the shared headers.
- `cmake/` has `CPM.cmake`, `SppScripts.cmake` (`spp_add_scripts`, `spp_script_host`), `SppTests.cmake` (`spp_gtest`) and `SppFormat.cmake` (the format library of `${}`).

## Build

The file `CMakePresets.json` has the presets. A build goes to `cmake-build/<preset>`.

    cmake --preset linux-clang-files
    cmake --build --preset linux-clang-files-dbg
    ctest --preset linux-clang-files-dbg

- `linux-clang-files` uses the backend `CLANG_FILES`. `linux-clang-jit` uses the backend `JIT`.
- Add `-asan` or `-tsan` to the preset name for a sanitizer build, for example `linux-clang-files-asan`.
- The build presets end with `-dbg` or `-rel`.
- The JIT backend also works with ThreadSanitizer. ORC and ThreadSanitizer do not conflict.
- `.clang-format` has a second document, `Language: CSharp`, for the S++ scripts. It uses the Microsoft style with the C++ choices on top (4 spaces, 120 columns, Allman braces).
- `tools/format-spp.sh [--check] files...` formats `.hot` files in place. `--check` changes nothing and lists the files that need formatting. clang-format has no `.hot` mapping, so the script uses `--assume-filename=<file>.cs`.
- Known limits: the formatter writes `using<map>;`, `std.string && s` (a qualified rvalue reference parameter) and `["a" : 1]`. It puts a lambda body on its own lines after `=>`. These are spaces only. Meaning stays the same. See DECISIONS.md.
- S++ supports C++17 and later. The generated code, `include/spp`, the runtime library and the backends build as C++17 or later.
- `sppc`, the language server and the tests need C++20. The build forces C++20 on these targets. The default standard of the other targets is 20. Set `-DCMAKE_CXX_STANDARD=17` for a C++17 host.
- The standard of the host (`SPP_CXX_STANDARD` in `include/spp/config.h`: 17, 20 or 23) is the standard of the script compile and of the parse of the native headers. The `sppc` flag `--std=c++17|c++20|c++23` overrides it.
- A C++17 host needs fmt for `${}`. See DECISIONS.md.
- String interpolation `${}` uses fmt in a C++17 build and `std::format` in a C++20 build. `-DSPP_USE_FMT=ON` selects fmt at C++20 and later. The choice is a build-time choice. `SPP_FMT_PROVIDER` says where fmt comes from: `AUTO` (default: `find_package`, then CPM.cmake), `SYSTEM` (`find_package` only), `CPM` (always fetch) or `NONE` (your project supplies the target `fmt::fmt-header-only` or `fmt::fmt`). See doc/cmake.md and doc/embedding.md.

### Build tree layout

The source tree has one `CMakeLists.txt` in each directory (`src/*`, `samples/*`, `tests/*`, `tools/*`). The build tree `cmake-build/<preset>` has these folders:

- `bin/<CONFIG>/` has the executables.
- `lib/<CONFIG>/` has the libraries and the fixture `.so` files.
- `spp_scripts/` and `spp_aot/` have the precompiled scripts.
- `tmp/` has the scratch files: the work directories, the directory of each test, the ImGui work directory and `imgui.ini`. ctest and the programs remove them by default.
- `tests/**` has the ctest discovery files.

To keep the scratch files, set the CMake option or the environment variable.

    cmake -DSPP_KEEP_TEMP=ON ...
    SPP_KEEP_TEMP=1 ctest --preset linux-clang-files-dbg

- The environment variable overrides the option. The value `0` or an empty value means off.
- The backend then keeps `tmp/spp_work/p<pid>`.
- Each test keeps `tmp/tests/<Suite>.<Test>/p<pid>`.
- ctest runs the fixture `spp_scratch_clean` first. It wipes `tmp/tests`, `tmp/spp_work` and `tmp/bench`, unless keep is on.

To purge an old build tree, remove the preset directory and configure again:

    rm -rf cmake-build/<preset>
    cmake --preset <preset>

## Samples

The folder `samples/` has seven projects: `aot`, `runtime-compile`, `precompiled-library`, `hot-reload`, `clang-files-backend`, `jit-backend` and `imgui`. See `samples/README.md` for the list. `doc/cmake.md` explains how a project uses S++ with CMake.

The option `SPP_BUILD_SAMPLES` is ON for the main project. The target names start with `sample_`:

    cmake --preset linux-clang-files
    cmake --build --preset linux-clang-files-dbg --target sample_aot_spp sample_aot_cpp
    ctest --preset linux-clang-files-dbg -R '^sample_aot_(spp|cpp)$'

A sample also builds as a separate project. It needs `clang`, `libclang` and Ninja. The default `SPP_ROOT` is the tree that holds the sample:

    cmake -S samples/aot -B /tmp/sample_aot -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=clang++
    cmake --build /tmp/sample_aot
    ctest --test-dir /tmp/sample_aot --output-on-failure

The option `SPP_SAMPLES_STANDALONE_TEST` (default OFF) adds the CTest test `samples_standalone_aot`. The test builds and runs `samples/aot` as a separate project in `samples_standalone/aot` of the build tree.

## Run

    ctest --preset linux-clang-files-dbg -R sample_hot_reload -V

The test runs `samples/hot-reload`. It loads a script, makes one object, reloads the script and keeps the live object. It exits with 0 on success. The test `HandwrittenGeneration` loads `gen1.so` in the same way (see "Test").

The test `ReloadMigration` uses the path of a library that does not exist (`gen3_missing.so`). The load must fail.

## Translator

    cmake-build/linux-clang-files/bin/Debug/sppc --emit-cpp tests/scripts/player_v1.hot --gen 1 -o out.cpp -I include -I tests/engine --slots player.slots

`sppc --emit-cpp` is a thin wrapper. It reads the files and calls `sppc::translate` from `sppc_lib` (`include/spp/sppc_lib.h`). The function takes strings: script text, script name for `#line`, generation, slot file content, include dirs and resource dir. It returns strings: C++ text, new slot file content and diagnostics. It does not write a file. The translator reads the script and the imported headers. It asks libclang (C API) for the virtual methods of the native base class. A `sppc::TranslatorSession` keeps the answer, see "Translator session". It writes the shell (one stub for each virtual), the state struct, the reflection table, the generation functions, the thunks and the record. It writes `#line` directives, so a Clang error points to the script line. It reports a missing pure virtual override and an override of a `final` method. A method with the name of a base virtual method and other parameter types is an error (it hides the virtual method).

The script has no brace initialization. Use parentheses: `T(args)` and `new T(args)`. A `{` in an expression gives an error. The script subset: `using`, `class N : Base`, fields `Type name;` or `Type name = init;` with `[alias("old")]`, methods `Ret name(T a) { ... }` (a method overrides a base virtual method when the name and the parameter types match; `void` means no return value), locals `Type name = e;`, `var name = e;`, `const Type name = e;` and `const var name = e;` (`var` gives `auto`), `this.`, `base.`, `null`, float literals, `Math.Min` as `Math::Min`, `if`, `while`, `for`, `return`, `throw`, `try`, `catch` and `finally`. A catch clause is `catch (Type e)` or `catch (...)`. A method can have a modifier: `override`, `virtual`, `new`, `public`, `private` or `protected`. A method that overrides a native virtual method needs `override`. The parser stores the access word and `virtual`. It does not enforce the access word, and `virtual` has no effect yet. A property is `Type N { get; set; }`, `{ get; }`, `{ get; private set; }` or `{ get { ... } set { ... } }`. The setter body reads the new value as `value`. A read or a write of `p.N` on another object of the class calls `get_N` or `set_N` of that object. A write gives the assigned value, so `a = p.N = 1` works. The rule uses the name only. If `p` is not an object of the class, the access throws `std::runtime_error`. Native stubs tell the translator how a C++ type behaves. `[native, handle("std::shared_ptr<$T>")] class Weapon;` declares a handle type. `$T` is the class. `[native, value] class std.string;` declares a value type. A pattern stub covers many types: `std.vector<*>` or `physics.*`. A stub lives in the script or in a prelude file. Give a prelude file to `sppc --prelude file` or in `TranslateOptions::preludes`. A handle type in a field, a local, a return type and `foreach` uses the storage type. A handle parameter becomes `const H&`. `.` on a handle uses `spp::deref`. `std::shared_ptr` is built in. A custom handle needs the `spp::is_handle` trait. A type with no stub keeps its raw text. `new T(args)` needs parentheses. It gives `std::make_shared<T>(args)` for a shared_ptr handle, `H(new T(args))` for another handle and `T(args)` otherwise. See `tests/scripts/handle_v1.hot` to `handle_v3.hot`. A reload that changes a field type is accepted (design 4.7): the `convert_<field>` hook converts the value, else the field gets its default value and the runtime logs a warning. See "Reload hooks". A handle stub on a native base class applies to all native subclasses. The nearest base wins. An exact stub on a subclass wins. A base stub beats a pattern stub. Only `handle` and `intrusive` are inherited. `value` is not inherited. The class hierarchy comes from libclang. A class in a system header is not in the map. A nested class and a template are not in the map. `intrusive("AddRef", "Release")` implies a handle: `[native, handle("$T*"), intrusive("AddRef", "Release")] class Object;`. If `handle` is given, it must be `$T*`. `intrusive` does not combine with `value`. The storage type is `spp::Intrusive<T, &Owner::AddRef, &Owner::Release>`. Owner is the class with the stub. A new object starts with zero references (the boost `intrusive_ptr` convention). The wrapper adds a reference when it is made from `T*`. The Add and Release methods must be non-overloaded, non-template members. A missing method gives a clang error that names it. A reload copies the field (add) and destroys the old block (release), so the count stays correct. See `tests/scripts/intrusive_v1.hot`, `intrusive_v2.hot` and `tests/engine/Object.h`. A script class is a handle. `class Name : Base { }` uses the handle of the native base. `class Name { }` has no base and is also a handle. `struct Name { }` is a value type inside the state block with copy semantics. `[value] class` on a script class is an error: use `struct`. A reload migrates it recursively. See `tests/scripts/kinds_v1.hot` and `kinds_v2.hot`. Deferred to slice 2: member access on a script handle, the borrowed-pointer checks of design 7.10.6, the checks of design 7.10.7, and the analysis of a parameter that a method stores as a copy. Native interface bases and their conflict checks are done (S8b, see "Native interface bases"). A comparison of an `Intrusive` with a raw `T*` is untested. Weak spot: a non-string attribute argument can give a spurious second error on a stub. A `T&` parameter takes its argument with no marker: `f(x)`. The prefix `&e` is the C++ address-of operator. It gives a borrowed pointer, and a field, a return value or a collection cannot hold it. `&h` on a handle is an error: use `h`, `&*h` or `*h`. `.` on a handle always means a member of the pointee, so `h.get()` calls `get` of the pointee. The arrow lambda `() => { ... }`, `(float x) => { ... }` and `(x) => { ... }` works in a method body. A parameter is `Type name`. A method parameter needs a type. A lambda parameter without a type is `auto`. The lambda body is a block or an expression: `(int x) => x * 2` is `(int32_t x) { return x * 2; }`. A C style cast `(T)e` works for a builtin type, `T*` and `T&`. A statement that starts with a chain with `?.` runs only when the receiver is not null: `h?.F(a);` and `h?.field = v;` give `if (h) ...`. The receiver runs once. `?.` in any other expression is an error. A parameter name has only one limit: the `__` prefix is reserved for generated code. See "Closures and unload".

Builtin type names are fixed-width. The parser changes each alias to one canonical C++ name, so `ulong` and `uint64` give the same text in the emitted C++, in the override key and in the reload type key. `int` becomes `int32_t`. The table is in `src/translator/builtin_types.h` and in `spp-design.md` (section 7.1.1). The names: `bool`, `sbyte`/`int8`, `byte`/`uint8`, `short`/`int16`, `ushort`/`uint16`, `int`/`int32`, `uint`/`uint32`, `long`/`int64`, `ulong`/`uint64`, `nint`/`intptr`, `nuint`/`uintptr`, `float`, `double`, `char` (8 bit) and `wchar`/`char16` (`char16_t`). The C++ multi-word spellings are errors with a hint: `unsigned int` gives "use 'uint'", and `long long` gives "use 'long'". A native name such as `int32_t` or `size_t` stays a normal native name.

Imports: `using "Actor.h";` becomes `#include "Actor.h"`. A standard header uses angle brackets: `using <vector>;` or `using "<vector>";`. Both forms become `#include <vector>`, in the generated file and in the libclang probe. A missing header gives an error with the script name and line. The generated file always includes `<cstddef>`, `<cstdint>`, `<cstdio>`, `<new>`, `<string>`, `<type_traits>`, `<typeinfo>`, `<utility>` and `spp/runtime.h`. Add `using` for any other standard header, for example `<algorithm>`. A script can call a non-inline function of the host, for example `Distance` in `tests/engine/Util.cpp`. The host executable must export it (`ENABLE_EXPORTS`, `-rdynamic`).

A script uses `.` for a scope and for a member access, for a pointer and for a reference. `std.string` becomes `std::string`. `p.m` becomes `spp::deref(p).m`, and the C++ compiler picks the pointer or the reference form. The text `::` and `->` in a script is an error: "use '.' instead of '::'" and "use '.' instead of '->'". The `=>` of a lambda stays. The old `->` return type is removed: the return type stands before the name.

A loop over a container is `foreach (Type x in c) stmt`. The head can also be `foreach (var x in c)`, `foreach (const Type x in c)` or `foreach (const var x in c)`. These words cannot be names of variables: `var`, `foreach`, `in`, `using`, `override`, `virtual`, `new`, `public`, `private`, `protected`, `get`, `set`, `finally` and `void`. A member name after a `.` can be any of them, for example `p.get()`.

`enumerate(xs)` and `range(n)` or `range(a, b)` are global functions of every script. `foreach (var (i, x) in enumerate(xs))` gives the index and the element. `foreach (int i in range(10))` counts. They are the last choice of the name rules: a local, a parameter, a member, a type or a native global of the same name wins.

An initializer is optional. With no initializer, a field, a property or a local has the default value: zero, `false`, null or the default constructor. `var` and `const` need an initializer.

Limits of the `.` access. The translator writes `a.b` as `spp::deref(a).b`. This works for a pointer and for a reference only. A type with `operator->` has no member access through `.`: a smart pointer, an iterator and `std::optional`. For a field `std.unique_ptr<Actor> p;`, `this.p.x` gives a Clang error "no member named 'x'". Use the unary `*` first: `var x = *it;` and `(*opt).name`. For a smart pointer that is not a handle, write `(*p).x`. A native global or a static data member, for example `Vec3.Zero.x`, becomes `Vec3::Zero.x`. The scope chain does not go through `spp::deref`. Copy the value to a typed local first. An array has no `.` access (the `deref` overload for an array is deleted).

The slot file (`--slots`) keeps the slot order of the script methods stable between generations. A generation can add a method, but it cannot remove one.

### C++ scripts

A script can be a plain `.cpp` file. Do not write `#include "spp/script.h"`: `sppc` injects the header itself. The hot-reload aids (`SPP_ALIAS`, `onReload`, `convert_*`) are temporary. After you remove them, the file is standard C++ and compiles in a normal CMake build with no `sppc`. A normal build of a file that still has aids needs `-include spp/script.h`. The extension selects the parser: `.hot` uses the custom syntax and `.cpp` uses libclang. Both modes load side by side in one program. A `.cpp` script cannot include a `.hot` script, and a `.hot` script cannot import a `.cpp` script. `sppc` keeps the method bodies as text and edits only a native base member, `this` as an argument, a module variable and a static member of the base. The fixtures are `tests/scripts/cpp_player_v1.cpp` and `tests/scripts/cpp_player_v2.cpp`. See [doc/cpp-scripts.md](doc/cpp-scripts.md) for the supported forms, the errors and the known limits. The language server and the VS Code extension serve `.hot` only. `spp_add_scripts` takes `.cpp` paths in `SCRIPTS`. A `foo.hot` and a `foo.cpp` in one call fail at configure time.

### Module declarations (S8a)

A script can declare enums, structs and module variables at file level (`DECISIONS.md` item 11). See `tests/scripts/module_v1.hot` and `module_v2.hot`.

- `enum Name [: type] { A, B = 5, C }` becomes `enum class Name : T`. The default type is `int`. `Name.A` becomes `Name::A`. A reload migrates an enum field as an integer.
- `struct Name { ... }` is the only way to declare a script value type. `[value] class Name { ... }` is an error: use `struct`. `[native, value] class X;` is a native stub and stays.
- A module variable is `[alias("old")] Type name [= init];` at file level. Every class of the script reads it by its bare name. The translator puts the variables in a synthetic struct `SppModule`. The loader owns one module block. A reload migrates it like the fields of a class.
- An initializer can use a module variable declared before it. The loader runs the initializers in declaration order.
- A convert hook of a module variable is `T convert_<var>(Old old) { ... }` at file level (`DECISIONS.md` item 14). It cannot read a module variable. See `tests/scripts/modconv_v1.hot` and `modconv_v2.hot`.
- Limits: a module variable is a native type, an enum or a struct. A script has no other free functions.

### Native interface bases (S8b)

A script class can have more than one native base: `class Boss : Actor, IDamageable, ITagged`. The first base is the primary base. The other bases are interface bases (design 4.4). See `tests/scripts/boss_v1.hot`, `boss_v2.hot`, `boss_rebase.hot` and `tests/engine/Damageable.h`.

- The shell derives from all bases: `struct Boss_Shell final : Actor, IDamageable, ITagged, spp::ShellBase`. The constructor arguments go to the primary base. An interface base needs a default constructor.
- libclang reads the virtuals of each base. Each virtual gets one slot. The slots of the primary base come first, then the slots of each interface base in the order of the list. A name in two bases gets a suffix of its parameter types, for example `SLOT_Size_i`.
- `override` works for a virtual of any base. A virtual without an override calls the implementation of its base, for example `IDamageable::Armor`. `base.Armor()` calls the interface implementation when the primary base has no virtual with that name.
- The record keeps the list of the bases in `base_name` (`"Actor, IDamageable, ITagged"`). The shell hash uses the list. A reload that adds or removes a base is rejected (design 5.3: RECONSTRUCT): "native interface bases changed".
- Errors: the same signature in two bases; an override that matches `F()` of one base and `F() const` of another base; an override of a `final` virtual; a `final` interface class; a missing pure virtual override; a base named two times; a script class as interface base; interface bases on a class with a script parent (design 4.5: the shell of a child adds no overrides; put the base on the root); an interface base without a default constructor.
- Limits: a child of a script class cannot add interface bases. It inherits the interface bases of the root of its family.

### Translator session

libclang needs about 400 ms to parse the engine headers. `sppc::TranslatorSession` (`include/spp/sppc_lib.h`) keeps the result: the virtual methods, the destructor and the constructors of each native base class. `sppc::translate(options, session)` uses it. `sppc::translate(options)` makes a temporary session, so it parses each time.

- The key is the include flags, the current directory and the set of imports. A script that names a base class that the entry does not have causes one new parse. The next calls for both bases hit.
- The entry keeps the path, the change time (ns) and the size of each file that the parse included (`clang_getInclusions`, with the system headers). Each call checks them with `stat`. If one differs, the session parses again. The check costs about 1 ms.
- The stamp of each file also has the inode, the ctime and a hash of the content. An edit that restores the old time and keeps the size changes the ctime or the inode. Then the check reads the content and compares the hash. A header that changed in the last 2 seconds is always hashed. The same check is in the polling mode of `harness::FileWatcher` (the script file).
- The cache does not see a new header that comes earlier in the include path when no shadow stamp covers it. Use `TranslatorSession::clear()` for odd cases.
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

- `-g` leaks. After a failed compile, Clang does not free the temporary debug metadata (about 270 KB for `bad.hot`, found with LeakSanitizer). `-gline-tables-only` does not leak, and it costs the same time. `JitOptions::debug_flag` sets the flag. LLJIT does not register a debugger plugin, so full debug info has no use now.
- `CompilerInstance::ExecuteAction` prints "N errors generated." to stderr when `ShowCarets` is on. The backend uses its own copy of the diagnostic options for the printer, so the text goes only to the result.
- `CompilerInvocation` keeps pointers into the argument strings. The strings must live as long as the compile.
- `LLVMConfig.cmake` probes C headers, so CMake calls `enable_language(C)` for the JIT build.
- The global constructors of a script (the class record) are run by `LLJIT::initialize`. Without it, `Player_record` has no value.

### Measurements

Machine: 32 threads, Linux 7.2, clang 23.1.3, `-O0`/`-O1` as noted, the page cache is warm. Tool: `cmake-build/<preset>/bin/Debug/bench_backends` (`tools/bench/bench_backends.cpp`, `std::chrono`). Times in ms for one `load` (translate + compile + load) of `player_v1.hot` (v1) and `player_v2.hot` (v2). "Cold" is the first load in a new process (a new session). "Warm" is the median of 5 later loads in the same process. Warm backends share one translator session, except in the rows "no cache" (`--no-cache`). Cold v1 always parses the headers. Cold v2 and all warm loads use the cache, except v1 in the rows "no cache".

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

CLANG_FILES with a PCH (P1, S19). The bench tool makes a PCH only in the JIT backend. For CLANG_FILES, make the PCH with the compiler and pass it through `--cxxflag`. The PCH header has the same includes as the one in `bench_backends.cpp`.

    clang++ -std=c++20 -fPIC -g -O1 -resource-dir=$(clang++ -print-resource-dir) -I include -I tests/engine \
        -x c++-header pch.hpp -o p.pch
    bench_backends --warm 9 --cxxflag -O1 --cxxflag -include-pch --cxxflag $PWD/p.pch

| Configuration (S19 run, same machine, other load) | v1 cold | v2 cold | v1 warm | v2 warm |
|---|---|---|---|---|
| CLANG_FILES, `-O1`, cache | 1693 | 1111 | 1110 | 1104 |
| CLANG_FILES, `-O1`, `-include-pch`, cache | 931 | 344 | 357 | 349 |

The PCH takes 1.1 s to make, once, and is 13.5 MB. It saves 755 ms of 1104 ms in a warm load (68%). The numbers of this run are higher than the rows above, because the machine had more load. Compare the two rows of one run only.

P1 criterion (design 8.2): under 2 s for one module after a one-line change, fail over 5 s. Result: pass in every configuration, also CLANG_FILES with no PCH (1.1 s). With a PCH: JIT 166 ms and CLANG_FILES 350 ms. The test headers are small. Measure again with the real engine headers.

Before the cache, the translate time was 411 ms in each load. The split of one translate (v2, `std::chrono` in `translate_impl`, measured before the change) was: libclang parse of the imports 406 ms, script parser 0.4 ms, visit of the base class 0.5 ms, text emit 2.7 ms. Only the libclang parse is large.

Translate alone (the bench prints it): 406 ms with a new session (miss), 1.3 ms with the same session (hit). The target was 50 ms.

Phases of one JIT load (v2, warm, cache): translate 1.4, compile 782, ORC (code generation, link, initializers, lookup) 19. With `-include-pch`: translate 1.4, compile 146, ORC 18. A new session parses the headers again, so v1 warm in the rows "no cache" costs 411 ms more.

- JIT against CLANG_FILES with the same `-O1`: about 140 ms less (15%). The saving is the process start and the link step. The compile itself is the main cost in both.
- `-include-pch` works with the in-memory compile. `JitBackend::make_pch(header, out)` makes the PCH in the process (886 ms once, 10 MB). The header must start with the same includes as the generated file. It saves 636 ms of 782 ms in the compile phase (81%) and 642 ms of 808 ms in a load (79%, with the translator cache). The PCH keeps the path and the time stamp of the header and needs the same flags (for example `-fsanitize`).
- Size: `backend_tests` is 1.29 MB (CLANG_FILES) and 1.69 MB (JIT), so +0.4 MB. Both builds already need `libclang.so` (45 MB) and `libLLVM.so` (173 MB) for the translator. The JIT build also needs `libclang-cpp.so` (78 MB). Peak RSS of the bench process: 100 MB (CLANG_FILES), 299 MB (JIT), 246 MB (JIT with PCH).

## Precompiled scripts

The tool `sppc` compiles scripts ahead of time. This is not a backend. Any build can load the result. Call `spp::module_from_library(path)` and give the `Module` to the `Loader`. This call needs no backend object. The sample [samples/precompiled-library](samples/precompiled-library/) is a complete project.

    sppc player_v1.hot --gen 1 -I include -I tests/engine --out-dir out/
    sppc player_v2.hot --gen 2 -I include -I tests/engine --out-dir out/

The tool `sppc` has two modes. The default mode builds a `.so`. The mode `--emit-cpp` only translates (see "Translator").

Command line of the build mode:

    sppc <script>... --gen N (--out-dir DIR | -o x.so) [-I dir]... [--resource-dir dir]
         [--cxx command] [--cxxflag flag]... [--slots file] [--no-strict-borrowed]

Command line of the translate mode. Without `-o`, the tool prints the C++ to the standard output:

    sppc --emit-cpp <script> --gen N [-o out.cpp] [-I dir]... [--prelude file]...
         [--slots file] [--resource-dir dir] [--no-strict-borrowed]

- `--raw-diagnostics` prints the compiler text as it is. By default `sppc` rewrites it (see "Diagnostics and debugging").
- `--no-strict-borrowed` makes the borrowed-storage findings (a stored `T*`, `T&` or `&x`) warnings. By default they are errors.
- The order of the classes in a script does not matter. A cycle of classes is an error.
- `<script>...` is one or more script files. Each script gives one library.
- `--gen N` is the generation number (1 or more).
- `-o x.so` builds one script into the library `x.so`. The C++ file goes to the directory of `x.so`. Use `-o` or `--out-dir`, not both.
- `--out-dir DIR` is the output directory. For a script `player_v1.hot`, `sppc` writes `DIR/player_v1.g1.cpp` and `DIR/player_v1.g1.so`.
- `-I dir` is an include dir. The translator and the compiler use it.
- `--resource-dir dir` is the Clang resource dir. The default is the one that the build found.
- `--cxx command` is the compiler command. The default is `clang++`.
- `--cxxflag flag` adds one flag to the compile command. Repeat it for more flags.
- `--slots file` is the slot file. The default is `DIR/slots.txt`. It keeps the slot order of the script methods between generations. Build the generations of one class in order.

The exit code is 0 for success, 1 for an error and 2 for a wrong command line. The tool prints the compiler errors at the script line. It does not need CMake.

The CMake function `spp_add_scripts` (`cmake/SppScripts.cmake`) only runs `sppc`:

    spp_add_scripts(TARGET player SCRIPTS tests/scripts/player_v1.hot
                    INCLUDES include tests/engine FLAGS ${flags} DEPENDS ${headers})

`TARGET` is a custom target that makes the libraries in `OUT_DIR` (default `${CMAKE_BINARY_DIR}/spp_scripts`). The generation is 1. `SPPC_ARGS` passes extra arguments to `sppc`. The tests use `SPPC_ARGS --gen n` and `AFTER` to build later generations.

## AOT compilation

An AOT build has no loader, no reload, no JIT and no Clang (design M7). The generated C++ of all scripts is part of the game binary. The objects are plain C++ objects with the same shell classes. The class records, the slot tables and the factories are constant data of the binary.

Command line (S12). The tool translates generation 1 of each script. It does not compile:

    sppc --aot <script>... --out-dir DIR [-I dir]... [--prelude file]... [--no-strict-borrowed]
         [--format=std|fmt] [--resource-dir dir]

- For each script `sppc` writes `DIR/<name>.aot.cpp`. The register function has the name `spp_script_register_<name>`, so many scripts link into one program.
- `sppc` also writes `DIR/spp_aot_registry.cpp`. It defines the constant table `spp::aot_table` (`include/spp/aot.h`).
- The script names must be different. The class names must be different over all the scripts of a program. An AOT script has no slot file and no later generation.

The CMake function `spp_add_scripts` with the option `AOT` (`cmake/SppScripts.cmake`) runs `sppc --aot` and builds a static library. `AFTER` does not work with `AOT`. `PRELUDES` works only with `AOT`:

    spp_add_scripts(TARGET game_scripts AOT SCRIPTS player.hot boss.hot
                    INCLUDES include tests/engine [PRELUDES file...] [DEPENDS file...])
    target_link_libraries(game PRIVATE game_scripts)

The library links `spp_aot`. This target holds the core of the runtime (`threads.cpp`, `registry.cpp`) and the AOT registry (`aot.cpp`). The loader, the migration and `dlopen` are in the other target `spp_runtime`. Do not link both targets in one program.

At start-up the program makes the registry. The registry calls the register function of each script, makes the blocks of the module variables and sets `spp::active_generation` to 1:

    spp::AotRegistry registry(spp::aot_table);
    Actor* boss = registry.create_as<Actor>("Boss");   // the factory: the constructor with default arguments
    world.Add(boss);                                    // the engine ticks it as before

`new` of a script class, script virtuals, properties, `is`/`as` and constructors work as under the loader, because the code is the same. The generation guard stays in the code and costs one compare. An AOT program never runs `onReload`.

The sample [samples/aot](samples/aot/) is a small project that consumes S++ with `add_subdirectory`. The test program is `tests/integration/aot/aot_demo_main.cpp` (target `aot_demo`) with the script `tests/scripts/aot_demo.hot`. The tests are in `tests/integration/aot/`. They run the same steps with the loader and with the AOT program and compare the text. They check with `nm` that `aot_demo` has no loader and no Clang symbol. A compile error in the AOT C++ shows the generated file, not the script line.

## Dependencies

CMake uses CPM (`cmake/CPM.cmake`) with `CPM_USE_LOCAL_PACKAGES ON`. CPM uses an installed package first. It fetches the package only when the package is missing.

- From the system only: libclang (C API), a Clang C++ compiler, and Threads. The JIT build also needs the LLVM and Clang development packages. CPM never fetches these.
- System first, CPM when missing: GoogleTest (v1.17.0), lexy (v2025.05.0), nlohmann_json (3.12.0).
- Always from CPM: Dear ImGui (v1.92.9b). It is fetched only when `find_package(SDL3)` succeeds. SDL3 comes from the system.

Options to force the fetch:

- `-DCPM_USE_LOCAL_PACKAGES=OFF` or `-DCPM_DOWNLOAD_ALL=ON` fetches every CPM package.
- `-DCMAKE_DISABLE_FIND_PACKAGE_GTest=ON` fetches one package. Use `lexy` and `nlohmann_json` for the others.
- `-DCPM_SOURCE_CACHE=<dir>` keeps the sources in one directory, so the next configure does not download again.
- `-DCPM_LOCAL_PACKAGES_ONLY=ON` never fetches. Configure fails when a package is missing.


## Test

    cmake --build --preset linux-clang-files-dbg && ctest --preset linux-clang-files-dbg

All tests are GoogleTest cases. CMake registers them with `spp_gtest` (a wrapper of `gtest_discover_tests`).

- `HandwrittenGeneration` (`tests/integration/reload/handwritten_generation_tests.cpp`) loads `gen1.so` and makes one `Player`. It ticks the world 3 times and calls both `TakeDamage` overloads. It checks the type name, the position and the field `hp` by reflection.
- `ReloadMigration` (`tests/integration/reload/reload_migration_tests.cpp`) binds a delegate to `heal` through a thunk. It reloads gen 2 at the frame boundary and checks the address, the migrated state, the new `Tick` and the delegate. It then tries gen 4 and a missing library. Both must be rejected, and gen 2 must keep running.
- `BackendTests` (`tests/integration/reload/`) use the backend that `SPP_BACKEND` selected. They run the F1 reload scenario (`tests/support/f1_scenario.h`): generation 1 then generation 2, the same address, `hp` becomes `health`, `shield` is 25 and a delegate calls the new `heal`. A bad script (`tests/scripts/bad.hot`) must give an error that names `bad.hot` and the line. The runtime must keep running after the error.
- `SessionCache` (`tests/unit/translator/session/session_cache_tests.cpp`) checks the cache: a hit gives the same output as a miss, a changed header (also a nested one) invalidates it, a second base class works in the same session, and the second translate takes less than 150 ms (the real value is about 1.3 ms).
- `JitTests` (`tests/integration/backends/jit_backend_tests.cpp`, only in a JIT build) check that no file appears and no child process runs. They also check exceptions in JIT code, a precompiled header and the life of a generation.
- `Closure`, `GenerationUnload` (`tests/integration/closures/`) test escaped closures and the unload of generations with `tests/scripts/timer_v1.hot` and `timer_v2.hot`. See "Closures and unload".
- `LambdaTranslation` (`tests/unit/translator/translate/lambda_translation_tests.cpp`) checks the output for an arrow lambda.
- The other translator tests are in `tests/unit/translator/`. Each folder is one test program: `parser`, `lowering`, `emit`, `types`, `diagnostics`, `session` and `translate`. The golden-output tests are in `tests/golden/`. The expected files are in `tests/golden/expected/`. Set `SPP_UPDATE_GOLDEN` to write them again.
- `PrecompiledTests` load the libraries that `spp_add_scripts` made with `sppc`, through `module_from_library`. They run the same F1 scenario. They run for each `SPP_BACKEND` value. One test runs `sppc` on `bad.hot` and checks the exit code and the error line.

## ImGui sample

`sample_imgui_spp` and `sample_imgui_cpp` (`samples/imgui/`) draw a Dear ImGui window. A script class draws the user interface. The script includes the real `imgui.h` (`using "imgui.h";`) and calls the ImGui API as it is: `ImGui.Begin("x")`, `ImGui.Text("n: %d", this.n)`, `ImGui.Checkbox("c", &this.on)`. A pointer parameter takes the address of a field. There is no wrapper layer.

Build. The target exists only when CMake finds SDL3 (`find_package(SDL3)`). Without SDL3 the configure step prints a STATUS message and the rest of the build is the same. CPM fetches Dear ImGui (v1.92.9b) and builds the static library `imgui_sdl3` with the SDL3 and SDL_Renderer backends. The executable links it as a whole archive, so a script can call every ImGui function.

    cmake --preset linux-clang-files
    cmake --build --preset linux-clang-files-dbg --target sample_imgui_spp sample_imgui_cpp

Run. The default script of `sample_imgui_spp` is `samples/imgui/script/demo.hot`. The default script of `sample_imgui_cpp` is `samples/imgui/script/demo.cpp`, the same script as plain C++.

    cmake-build/linux-clang-files/bin/Debug/sample_imgui_spp [script]
    cmake-build/linux-clang-files/bin/Debug/sample_imgui_cpp [script]

Hot reload. The host checks the modification time of the script file at each frame. When it changes, the host compiles the script as a new generation and starts a reload. The commit happens after `Frame()`, at the frame boundary. The object keeps its field values. If the compile or the reload fails, the window "Script error" shows the text and the old generation keeps running. If `Frame()` throws, the host drops the object and shows the message. The next good reload makes a new object. The host writes `reloaded gen N` and `reload failed: ...` to stderr.

The base class is `UiApp` (`samples/imgui/UiApp.h`, virtual `Frame()`). Gap: a native array field such as `float rgb[3];` gives "expected a member name", so `ImGui.ColorEdit3("c", this.rgb)` does not work. Use `ImVec4 col` and `&this.col.x`, or three float fields.

## Reload

`Loader::request_reload` validates a `Module` with no side effect: `script_register`, base class, shell layout hash, slot count. `Loader::commit_pending` runs at the frame boundary (the `World` hook). It allocates all new state blocks, migrates, swaps the table and layout, sets `active_generation` and frees the old blocks.

## Closures and unload

An arrow lambda becomes `spp::make_closure(GEN, "Class::fn lambda", [=](params) { ... })`. It captures by value. `this` is the pointer `__self`. The body reads the state pointer again, so a lambda never keeps a pointer to a state block. The result is a `spp::Closure<Sig>`. It holds one pin of its generation (`spp::pin`, `unpin`, `pin_count`; the counts are atomic). Each call checks the generation. When the generation is not live, the call logs a stale call, counts it (`spp::stale_call_count()`) and returns a default value. The body does not run. A `Closure` converts to `std::function` and to an engine delegate that has `Bind(std::function<Sig>)`.

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

See `tests/scripts/hooks_v1.hot` to `hooks_v3.hot`, `tests/unit/runtime/migration_tests.cpp` and `tests/integration/reload/hook_tests.cpp`.

## Diagnostics and debugging

Diagnostics (S11, design 4.2 and 4.12):

- `sppc::rewrite_diagnostic(text, scripts, generated)` is a pure function in the translator library. It changes the Clang text to script names (`Player_Shell` to `Player`, `Player__heal` to `Player.heal`, `int32_t` to `int`). It does not count the hidden `__self` argument. It gives a plain message for the common mistakes (the table is in `src/translator/diag_rewrite.cpp`). It shows the script line in place of the generated line.
- Both backends and `sppc` use it. `raw_diagnostics` in the backend options and `sppc --raw-diagnostics` keep the compiler text.
- `#line` has no column in standard C++, so the generated C++ keeps `#line N "file"` only. The rewriter takes an optional `SourceMap` (the backends pass `TranslateResult::source_map`). It finds the generated line from the `#line` directives, maps the Clang column through the map, and prints the real script column. If the map has no segment for the column, the column is 1. Without a map the column is 1.
- `sppc::rewrite_translator_diagnostic(text, scripts)` gives the translator errors (parse, check, libclang facts) the same final form: `script:line:col: error: message`, the script line in the gutter form, and the count line (`1 error generated.`). `sppc` and both backends use it. `--raw-diagnostics` keeps the plain translator text.
- The backends compile with `-Werror=return-type`. A missing `return` is an error.
- The translator errors do not pass through the rewriter.
- The golden cases are in `tests/golden/diagnostics/` (21 mistakes). The test is `DiagnosticMistakes`.

Debug information:

- The generated C++ has `#line` directives. The JIT and `sppc` use `-gline-tables-only`, so the DWARF line table names the `.hot` file. The tests `DebugInfo.*` check this with `llvm-dwarfdump`.
- The JIT backend registers each object with the debugger through the GDB JIT interface (`llvm::orc::enableDebuggerSupport`). `JitOptions::debugger_support` turns it off. `debugger_support()` and `debugger_error()` tell the result. The test `JitTests.DebuggerGetsObjectWithScriptLines` reads `__jit_debug_descriptor`. LLVM 23 has no `createJITLoaderGDBRegistrar`.
- `tools/lldb/spp_formatters.py` and `tools/lldb/spp.lldbinit` show state blocks, shells and handles by script name. The command `spp-bt` prints the script frames. A synthetic child provider hides `__spp_owner` from the state blocks. `tools/natvis/spp.natvis` does the same for Visual Studio.
- The pure Python helpers have a test (`PythonLldbFormatters`, `python3 -I`). lldb itself, a real debugger session and Visual Studio were NOT run. The F3 check (breakpoint in `heal`, step, inspect `this.health`) stays open.

### Debugging

What the debugger shows:

- Each script class has a state struct, for example `PlayerState_g1`. Its first member is `spp::ShellBase* __spp_owner` (offset 0, with a `static_assert`).
- Script methods are members of the state struct. A frame shows `this` (the script fields) and the parameters. The locals `__self` and `__st` have the attribute `SPP_NODEBUG_LOCAL` (`[[gnu::nodebug]]` on Clang and GCC).
- A slot holds a pointer to the state member (`spp::SlotFn`, for example `PlayerState_g1::Tick`). `spp::call<Sig>(__self, slot, args...)` calls the member. No `Player__Tick` wrapper exists. A script function, a constructor and a destructor keep a hidden `SPP_HIDDEN` wrapper for direct script calls.
- The member prologue has the stale-generation check. A parameter with a body type has the name `__<name>` in the member. The first body line copies it to `<name>`.
- A line breakpoint on a `fn` line hits once. Step-in from the engine reaches the member after the frame of the shell override. The shell override maps to the class line.
- Every generated line that makes code has a `#line N "script.hot"` before it. Breakpoints and stepping use the `.hot` lines.
- A name that starts with `__` is internal state. A script field with such a name is a compile error.

For runtime header authors:

- `include/spp/runtime/hidden.h` has two macros. `SPP_HIDDEN` is `always_inline` and `nodebug`: the debugger never sees the function. `SPP_NODEBUG` is `nodebug`: the debugger steps over the function.
- Put one of them on each runtime helper, so that step-in does not enter it.
- Clang rejects `#pragma clang attribute` with `nodebug`. The C++ standard library functions (`c_str`, `shared_ptr`, `make_shared`) still show on step-in at `-O0`.

Editors:

- `editors/vscode/package.json` has `contributes.breakpoints` for the language `spp`. The C++ extension (`cppdbg` with gdb, or lldb) works with no custom launch configuration.
- Option: the gdb setting `skip -gfi "*/include/c++/*"` hides the libstdc++ stops.
- `tools/natjmc/spp.natjmc` is a Just My Code file. It marks `spp::*`, the shell, the thunks, the virtual helpers, the base forwarders and `state_of` as non-user code. Status: unverified. Visual Studio reads it from the user Visual Studio Visualizers folder. `cppvsdbg` in VS Code may not read it.
- MSVC and Visual Studio are untested. The MSVC code paths are unverified: `SPP_NODEBUG_LOCAL` in `hidden.h`, `spp::aligned_alloc`, the `LoadLibrary` branch in `module.cpp` and `/JMC` in CMake.

## Language server

`spp-ls` (S14b, design 10.1, M12 to M14) is a stdio LSP 3.17 server. It serves `.hot` files only. A `.cpp` script is a C++ file for clangd (see [doc/cpp-scripts.md](doc/cpp-scripts.md)). The code is in `src/ls/`. The extension in `editors/vscode/` starts it.

### Editor setup: two servers (S31, DECISIONS 18)

An editor uses two language servers on one project:

- The official clangd extension serves the C++ files. It reads the `compile_commands.json` of the project build.
- The `spp` extension and `spp-ls` serve all features for the `.hot` files. For a `.cpp` script that a host lists, they show only the errors of `sppc` as diagnostics. `spp-ls` is not a transparent clangd proxy. It does not answer for C++ files.

Steps:

1. Install the clangd extension (`llvm-vs-code-extensions.vscode-clangd`) and `clangd`. Set it up for the C++ build as usual.
2. Build `spp-ls` and install the extension in `editors/vscode/`. Put the `spp-ls` arguments in `spp.server.args`. The include dirs come from the script hosts (next subsection). The flags `-I` and `--cxxflag` still work as overrides.
3. Optional, for workspace symbols and the background index of scripts: run `sppc <file.hot>... --gen N --out-dir DIR --compile-commands CDIR`. Give `--compile-commands-dir CDIR` to `spp-ls`.
4. Add `.cache/` to the ignore list of the project (for example in `.gitignore`).

Both servers can use the same project and the same `.cache/` directory. They do not disturb each other:

- The C++ clangd stores its index in `<dir of compile_commands.json>/.cache/clangd/index/`.
- The clangd child of `spp-ls` has its own database directory `.cache/spp/clangd/` in the first workspace folder. The directory has a link to the real `compile_commands.json`. So its index is in `.cache/spp/clangd/.cache/clangd/index/`. The extra nesting is accepted. `XDG_CACHE_HOME` does not move this index (see DECISIONS 18).
- The symbol index of `spp-ls` is in `.cache/spp/index/`.
- The C++ clangd does not get an open `.hot` document, and `spp-ls` does not get a C++ document. A diagnostic of one server never shows in the files of the other.

### Automatic setup (script hosts)

A script host tells `spp-ls` how the engine compiles its scripts. CMake writes the host files. So the user does not pass `-I` flags.

```cmake
include(cmake/SppScripts.cmake)
# spp_add_scripts calls spp_script_host by themselves.
# Call it directly for a host without these functions:
spp_script_host(NAME engine TARGET engine
                SCRIPT_DIRS ${CMAKE_SOURCE_DIR}/scripts
                INCLUDES ${CMAKE_SOURCE_DIR}/engine)
```

- At generate time, `spp_script_host` writes `${SPP_HOSTS_DIR}/<NAME>.json`. The default is `<source dir>/.cache/spp/hosts`. The file has the explicit include dirs, the script dirs and the scripts, with a full JSON escape, and `sourceDir` (the source directory of the call). The defines, options and include dirs of `TARGET` go to the side files `<NAME>.defines`, `<NAME>.options` and `<NAME>.includes`, one entry on each line. The JSON file names them in `definesFile`, `optionsFile` and `includesFile`. A relative path is relative to `sourceDir`, and `spp-ls` makes it absolute and normalizes it. `spp-ls` drops a repeated include dir. The order is the project include dir, the dirs of `TARGET`, then the explicit `INCLUDES`. Every valid include path works: spaces, quotes, backslashes, non-ASCII characters, `$`, `#`, brackets, `,`, `>`, `|`, `=`, a leading `-`, `..`, a trailing slash, relative paths, and dirs from imported targets or generator expressions. Two cases do not work. A path with a newline cannot be in a one-line-per-entry file. A path with a semicolon is a CMake list separator, so CMake splits it in two entries before `spp_script_host` sees it. The server passes each flag to clangd as one item of an argument array, so no shell escape is needed.
- `spp-ls` reads the host files from `<root>/.cache/spp/hosts`. The argument `--hosts-dir` (init option `hostsDir`) sets another directory.
- A script matches the host with the exact `scripts` entry. Else it matches the host with the longest `scriptDirs` prefix. A tie goes to the lower name.
- The effective options are the host values, then the explicit `-I` and `--cxxflag` values. So the flags override the host.
- A host is skipped when its `binaryDir/CMakeCache.txt` is missing. The server checks it again later.
- With no host, the old behavior stays. The server also shows an Information diagnostic at 0:0. It does not show it when `-I` or `--compile-commands-dir` is set.
- The server reloads the host files when the mtime of the hosts dir changes. The VS Code extension watches `**/*.hot` and the host files.
- Limits: many build dirs share one hosts dir, so the last configure wins. The server does not canonicalize symlinked workspace paths. Multi-Config generators are not tested.

How it works:

- The server owns the text of each open `.hot` document. On each change it runs `sppc::analyze()`. It gets the parse errors, the facts (classes, members), the generated C++ and the source map.
- It starts a stock `clangd` as a child process. It gives the generated C++ to clangd as the virtual file `<script>.hot.cpp`. The compile command comes from `spp::compile_command`, through `workspace/didChangeConfiguration`.
- It maps each position between the script and the generated C++ with the source map. A line without a segment uses the `#line` directives.
- `sppc::rewrite_generated_names` and `sppc::rewrite_clang_message` change generated names to script names in hover text, diagnostics and completion labels.
- JSON comes from nlohmann_json (CPM).

Features:

- Diagnostics: the parse errors of analyze, and the clangd errors at the script position.
- Completion works on any identifier prefix, with no trigger needed. The trigger characters are `.` and `::` (the client sends `:`, and the server ignores a single `:`). The list has: locals and parameters in scope, module variables, script enums, classes and structs, the enumerators after `Mode.`, the script members (own and inherited from script bases) after `this.` or after a variable of a script class, the native types, functions and members from clangd, and the S++ keywords and builtin types from a fixed list. All names are in script form (`std.string`). The helpers of the translator (thunks, `SLOT_`, `_Shell`, plumbing) are not shown. The list is filtered by the typed prefix.
  - The script facts come from a token scan of the text. It works on a script with syntax errors. For clangd, the server puts the typed prefix and the placeholder `__spp_complete` in place of the word at the cursor. It closes the open brackets of an incomplete statement and translates again with the tolerant parse. Then it asks clangd at the placeholder.
  - If the translation fails, the list has the keywords and the script facts, without clangd.
  - `spp/indexStats` also gives `completions`, the number of completion translations. A test uses it to check the reuse.
  - The server keeps the last clangd answer. While the user types more characters of the same word, and the rest of the text does not change, it filters the answer and does not translate again. A failed translation is kept the same way.
- A clangd error in the code of an imported script is published for the imported script, at its own line and column. The importing document does not get it. An open script keeps its own diagnostics.
- Signature help (`(` and `,` trigger) asks clangd at the generated call. The active parameter is the number of commas before the cursor. The labels have script names.
- Formatting (`textDocument/formatting`) sets the indent from the braces, adds one level inside open brackets, removes trailing white space and ends the text with a new line. Strings, chars and comments stay. The result is one edit, or none when the text is already formatted.
- Code actions (quick fix): insert a missing `;` for the error `expected ';'`, and replace a word with the name from a clangd `did you mean '...'` message.
- Hover, go-to-definition and references. A script symbol goes to the script. A native symbol goes to the native header.
- Document symbols come from analyze. Semantic tokens come from a small lexer and the facts.
- Rename changes the script symbols (class, member, field) in all `.hot` files of the workspace. The server scans the workspace folders from `initialize` (it skips hidden folders and `node_modules`). A class or a member with the same name in another script (for example the versions of a script) gets the new name there too. An open document uses its editor text. A file on disk goes to clangd only for the request. A class name has no segment in the source map, so the semantic tokens find its places. References also look in the other `.hot` files. Rename of a native symbol gives the error `cannot rename a native symbol: X`.
- The shard digest covers the text of the file and the texts of the scripts that it imports (also the imports of the imports). A changed import makes the shard stale. The saved symbol index speeds up rename and references on closed `.hot` files. There is no limit on the number of files. It has one shard for each `.hot` file. All shards are flat in `.cache/spp/index/` in the first workspace folder. `--index-dir dir` sets another directory. The shard name is a hash of the file path, so a new shard replaces the old shard and the index does not grow when a file changes. A shard holds a format version, the content digest, the classes and members, and the places of each class. The places of a member come from clangd. The server adds them to the shard at the first rename. The server loads a shard only if the version and the digest match. Else it analyzes the file again and writes the shard again. A write goes to a temporary file, then a rename puts it in place. At `initialize` and at each scan the server deletes the shards of missing files. The index is filled when a rename or a references request scans a closed file. It is not filled at startup. Add `.cache/` to the ignore list of the project. The request `spp/indexStats` gives the counts (`analyzed`, `loaded`, `stored`).
- Workspace symbols come from the clangd index. Give `--compile-commands-dir` with the output dir of `sppc --compile-commands`. Then clangd runs its background index on the generated files. A symbol of a generated file maps to its script member. The helpers of the translator (thunks, slot functions) are not shown.

The `#line` check: clangd reports the real line of the generated file, not the `#line` line. So the generated C++ keeps its `#line` directives. No translator option is necessary.

From C++ to the script:

- In a `.cpp` file from `sppc --compile-commands`, the `#line` directives name the `.hot` file. A debugger and the compiler use them. clangd does not use them for navigation.
- The custom request `spp/toScript` (params: `textDocument`, `position`) gives the script location of a position in a generated file. It uses the source map for an open script, and the `#line` directives for a file on disk.
- The test `LanguageServer.GeneratedFileOnDiskMapsBackToTheScript` checks it.

Limits:

- After a translation error, clangd keeps the last good generated C++. The server keeps the script text of that C++ and maps each position through the edit (common prefix and suffix). A position inside the edit has no mapping. The old clangd diagnostics are dropped until the next good translation.
- clangd does not publish again when a new version has the same diagnostics. The server keeps the last raw clangd diagnostics of each document. After each edit it maps them again to the new script text. A new publish of clangd replaces them.
- A `#line` fallback has the line only. The column is the column of the same word in the script line, or the first non-blank column.
- Two open scripts that import the same script publish diagnostics for it. The last publish wins.

The tests are in `tests/integration/ls/` (`LanguageServer.*`). They run the server on a thread with an LSP client over pipes. The clangd tests are skipped when `clangd` is missing. Set `SPP_LS_CLANGD_LOG=file` to get the clangd log of a test.

## Threads and the safe point

The tests are in `tests/integration/threading/parallel_tick_tests.cpp` (suite `ParallelTick`).

`World::TickParallel(dt, threads)` ticks the actors on worker threads. The workers start once. They stop when the world ends. Each worker ticks one chunk of the actor list. The main thread waits at a barrier. After all workers reach the barrier, the main thread runs the frame boundary hook. The workers stay idle until the hook ends.

The reload commit runs only in this hook. At that time no thread is in script code. The barrier also orders the memory, so the workers see the result of the commit.

A compile thread can call `Loader::request_reload` and `Loader::reload_pending` at any time. These functions take the mutex of the loader. They only validate. They change no live class and no live object. `commit_pending` takes the same mutex.

The debug check is the counter `spp::in_script_tick()`. The engine wraps each actor tick in a `spp::ScriptTickScope`. `commit_pending` asserts that the counter is zero. It also counts a violation in `spp::unsafe_commit_count()`. The check exists in debug builds only. With `NDEBUG`, `spp::kTickChecks` is false, `ScriptTickScope` does nothing and the commit has no check.

A class that a new generation drops is removed (design 6.1). Its record stays. Its live objects keep their state. Each slot points to `spp::removed_class_handler()`, which logs the first call, counts all calls and returns zero. A floating point result of such a call is not defined. The class can come back in a later generation. `Loader::stats()` and `Loader::stats_text()` (the `script_stats` text) list the generations, the pins, the live objects, the classes with their slots and the removed classes.

The sample `samples/hot-reload` shows the reload path. The test host `tests/engine/Harness` ticks a world and reloads a script when the file changes (`FileWatcher`: inotify on Linux, polling else). The tests in `tests/integration/reload/` (`integration_reload_tests`) test this.

The warnings of a reload use readable names: C++ type names (`int`, `std::string`) and class names (`Hero`, not `HeroState_g2`).

Rules for threads:

- A worker reads class records and state blocks. It writes the state of its own actors only.
- A worker does not create or destroy script objects. It does not call the loader.
- The main thread calls `load`, `commit_pending`, `frame_boundary` and `stats`.
- Any thread can pin or unpin a generation.

Test results: `R5_CommitOnlyAtBarrier` runs 20 seeds with 1000 actors and 2 workers. `R5_RejectedReloadFromOtherThread` runs 5 seeds. Both tests pass in the normal, ASan and TSan builds. They also pass with `--gtest_repeat=20` in the normal and TSan builds. TSan reports no race. When the lock in `request_reload` is removed, TSan reports a race. So the test can find this kind of error.

### Entry counters: a commit without a frame boundary

Some engines have no frame boundary. For them, the loader has a second mode (design 6.3):

```cpp
loader.set_sync_mode(spp::SyncMode::EntryCounters);  // default: spp::SyncMode::StopTheWorld
```

The mode applies to the whole process. Set it when no thread is in script code. The destructor of the `Loader` sets `StopTheWorld` again.

How it works:

- Each script entry is a `spp::ScriptEntry`. The entries are the slot calls (`spp::call`: shell stubs, thunks and script virtual calls), the closure calls and the script destructors.
- The outermost entry of a thread increments one global count. A nested entry (script, then native, then script) increments only a thread-local depth. So a nested entry never blocks.
- `commit_pending` opens a `spp::CommitWindow`. It closes the gate and waits until the count is zero. A new outermost entry waits at the closed gate. Then the commit runs. After the commit, the gate opens and the waiting threads continue in the new generation.
- The commit thread can run script code in the window, for example an `onReload` hook.
- `commit_pending` can run on any one thread. It fails with an error when the calling thread is in script code. `frame_boundary` can run on the commit thread after the commit.

Cost: in the default mode, each entry reads one global flag (a relaxed atomic load) and takes no branch. Define `SPP_NO_ENTRY_COUNTERS` to remove the check at compile time. In the `EntryCounters` mode, each outermost entry does one atomic increment and one atomic decrement of a shared counter.

Limits:

- A deadlock occurs when script code waits for a thread that waits at the gate, or for the end of the commit.
- The creation of a script object through the factory (`ClassEntry::create`) is an entry (S27), so a commit cannot swap the class during the construction. Native code that reads the fields of an object holds a `spp::ScriptEntry`. `new` in script code runs inside a script entry. Object creation and destruction stay on one thread at a time.
- A thread that entered script code in the default mode is not counted.
- `ScriptTickScope` and its debug check apply to the default mode only. A worker can wait at the gate inside a `ScriptTickScope`.

Diagnostics: `spp::active_entries()`, `spp::entry_depth()`, `spp::commit_window_closed()`, `spp::commit_window_count()` and `spp::blocked_entry_count()`.

The tests are in `tests/unit/runtime/entry_counter_tests.cpp` and `tests/integration/threading/entry_counter_tests.cpp` (suite `EntryCounters`). In the integration test, 4 worker threads call `Tick` on 64 actors in a loop. Another thread commits 8 reloads at the same time. Each `Tick` calls the native `Util::Probe`, and the probe calls the script `TakeDamage`. The test checks that no call is lost, that no call runs on a half migrated object, and that a nested call during a waiting commit does not deadlock. The unit test opens 2000 commit windows while 4 threads enter.
