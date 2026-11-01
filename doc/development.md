---
layout: default
title: Development notes
nav_order: 44
---

Notes for people who work on Hot++ itself: the milestones, the source layout, the tests, the translator and the `hotc` command line.

## Status

Each row is a milestone. "Not testable here" means that the work needs a part that the development machine does not have.

| Milestone | Status | Note |
|---|---|---|
| M0 Spikes | Partial | R0 and P1 are done with the test host `tests/engine`. The spikes for risk 2 and 3 need the real engine: not testable here. |
| M1 Runtime spine | Done | R1 to R6 pass, also under ASan and TSan. |
| M2 Translator v0 | Done | F1 passes, also for a native base that derives from a class template instance, a template over a template and a partial specialization (design risk 8). `class Foo : Tmpl<int>` works. |
| M3 Diagnostics and debugging | Partial | F2 passes. F3 passes with `gdb`. `lldb`, the `lldb` formatters in a live session and Visual Studio with `natvis` are not testable here. |
| M4 Engine integration | Not testable here | The test host `tests/engine` and the samples in `samples/` are the only hosts. A real engine is needed. |
| M5 Contract enforcement | Done | The rows of design 5.3 have tests in `tests/integration/reload/` and `tests/unit/translator/translate/`. |
| M6 Language v1 | Done | Inheritance, virtuals, `is`/`as`, lambdas, enums, structs, module variables, interpolation, constructors. `async` methods and `await` (C++20). |
| M7 AOT compilation | Done | `hotc --aot`. Generation 1 only. |
| M8 Unloading | Done | Core module. Generation 1 unloads. 1000 reloads with `HOT_STRESS=1`. |
| M9 Optional ORC backend | Partial | The JIT backend works on Linux. The debug-symbol resolver for Windows is not testable here. |
| M10 Source map and tolerant parse | Done | `TranslateResult::source_map`, `hotc::analyze`. |
| M11 VS Code extension | Done | Grammar snapshot test and package. A real VS Code session is not testable here. |
| M12 Language server stage 1 | Done | `hot-ls`, a server that uses a stock `clangd` as a child process (no fork). |
| M13 Language server stage 2 | Done | One test for each feature. |
| M14 Language server stage 3 | Done | Background index, `hot/toScript`, rename across the workspace, `hotc --compile-commands`. |

Not done, and not testable here: Windows and macOS, a real engine, a real `lldb` and Visual Studio session, the optional ORC debug-symbol resolver.

## Source layout

- `include/hot/` has the public headers: `runtime.h`, `module.h`, `backend.h`, `clang_files_backend.h`, `jit_backend.h` and `hotc_lib.h`. Use the form `#include "hot/<name>.h"`.
- `src/` has the library code. `src/runtime/` has the script runtime and the backends. `src/translator/` has the translator library `hotc_lib`. `src/tools/` has the tool `hotc`.
- `tests/engine/` is the test host (`Actor`, `World`, `Harness`, `FileWatcher`). It is not a model for a user project: copy from `samples/`. `tests/scripts/` has the test scripts. `tests/integration/aot/` has the program `aot_demo`. `tests/integration/fixtures/` has the hand-written generations. `tools/bench/` has the benchmark `bench_backends` (`bench_backends.cpp`, the scripts in `scripts/`).
- `samples/` has seven self-contained projects for consumers of Hot++ (see "Samples"): six show one way to host Hot++ each, and `imgui` is a Dear ImGui demo. Each one shows the `add_subdirectory` pattern. With `-DHOT_ROOT=` and `CMAKE_PREFIX_PATH` each one uses an installed SDK (`find_package(Hot)`) instead.
- `tests/` has the GoogleTest cases and the shared headers.
- `cmake/` has `HotConfig.cmake.in` (the package config of the installed SDK), `SdkInstallTest.cmake`, `CPM.cmake`, `HotScripts.cmake` (`hot_add_scripts`, `hot_script_host`), `HotTests.cmake` (`hot_gtest`) and `HotFormat.cmake` (the format library of `${}`).

## Tests

    cmake --build --preset linux-clang-files-dbg && ctest --preset linux-clang-files-dbg

All tests are GoogleTest cases. CMake registers them with `hot_gtest` (a wrapper of `gtest_discover_tests`).

- `HandwrittenGeneration` (`tests/integration/reload/handwritten_generation_tests.cpp`) loads `gen1.so` and makes one `Player`. It ticks the world 3 times and calls both `TakeDamage` overloads. It checks the type name, the position and the field `hp` by reflection.
- `ReloadMigration` (`tests/integration/reload/reload_migration_tests.cpp`) binds a delegate to `heal` through a thunk. It reloads gen 2 at the frame boundary and checks the address, the migrated state, the new `Tick` and the delegate. It then tries gen 4 and a missing library. Both must be rejected, and gen 2 must keep running.
- `BackendTests` (`tests/integration/reload/`) use the backend that `make_default_backend` returns (the environment variable `HOT_BACKEND` chooses it). They run the F1 reload scenario (`tests/support/f1_scenario.h`): generation 1 then generation 2, the same address, `hp` becomes `health`, `shield` is 25 and a delegate calls the new `heal`. A bad script (`tests/scripts/bad.hot`) must give an error that names `bad.hot` and the line. The runtime must keep running after the error.
- `SessionCache` (`tests/unit/translator/session/session_cache_tests.cpp`) checks the cache: a hit gives the same output as a miss, a changed header (also a nested one) invalidates it, a second base class works in the same session, and the second translate takes less than 150 ms (the real value is about 1.3 ms).
- `JitTests` (`tests/integration/backends/jit_backend_tests.cpp`, only in a JIT build) check that no file appears and no child process runs. They also check exceptions in JIT code, a precompiled header and the life of a generation.
- `Closure`, `GenerationUnload` (`tests/integration/closures/`) test escaped closures and the unload of generations with `tests/scripts/timer_v1.hot` and `timer_v2.hot`. See "Closures and unload".
- `LambdaTranslation` (`tests/unit/translator/translate/lambda_translation_tests.cpp`) checks the output for an arrow lambda.
- The other translator tests are in `tests/unit/translator/`. Each folder is one test program: `parser`, `lowering`, `emit`, `types`, `diagnostics`, `session` and `translate`. The golden-output tests are in `tests/golden/`. The expected files are in `tests/golden/expected/`. Set `HOT_UPDATE_GOLDEN` to write them again.
- `PrecompiledTests` load the libraries that `hot_add_scripts` made with `hotc`, through `module_from_library`. They run the same F1 scenario. They run with each backend. One test runs `hotc` on `bad.hot` and checks the exit code and the error line.

## CI

`.github/workflows/ci.yml` runs on each push and pull request to `master`. The job `linux` builds with the preset `linux-clang-files` (Clang 23 from apt.llvm.org) and runs all tests. The job `windows-msvc` builds with the preset `vs-files` and MSVC on `windows-latest` and runs the tests. Each job then makes a portable SDK (`HOT_PORTABLE=ON`, no tests). The smoke jobs unpack each SDK on a clean runner and run `tests/sdk_smoke/run.cmake`. The Windows smoke test uses MSVC and the Visual Studio generator. A push to `master` moves the tag `latest` and replaces the assets of the release `latest`. See [Download the SDK](download.html).

## Translator notes

- The slot file (`--slots`) keeps the slot order of the script methods stable between generations. A generation can add a method, but it cannot remove one.
- A custom handle type needs the `hot::is_handle` trait. `std::shared_ptr` is built in.
- `intrusive("AddRef", "Release")` implies a handle. The storage type is `hot::Intrusive<T, &Owner::AddRef, &Owner::Release>`. A new object starts with zero references (the boost `intrusive_ptr` convention). The wrapper adds a reference when it is made from `T*`. A reload copies the field (add) and destroys the old block (release), so the count stays correct. See `tests/scripts/intrusive_v1.hot`, `intrusive_v2.hot` and `tests/engine/Object.h`.
- A handle stub on a native base class applies to all native subclasses. The nearest base wins. An exact stub on a subclass wins. A base stub beats a pattern stub. Only `handle` and `intrusive` are inherited. `value` is not inherited.
- The class hierarchy comes from `libclang`. A class in a system header, a nested class and a template are not in the map.
- The generated file always includes `<cstddef>`, `<cstdint>`, `<cstdio>`, `<new>`, `<string>`, `<type_traits>`, `<typeinfo>`, `<utility>` and `hot/runtime.h`. Add `using` for any other standard header.
- A script can call a non-inline function of the host. The host executable must export it (`ENABLE_EXPORTS`, `-rdynamic`).
- A module variable is a native type, an enum or a struct. A convert hook of a module variable is `T convert_<var>(Old old) { ... }` at file level and cannot read a module variable. See `tests/scripts/modconv_v1.hot` and `modconv_v2.hot`.

### Native interface bases

A script class can have more than one native base: `class Boss : Actor, IDamageable, ITagged`. The first base is the primary base. The other bases are interface bases (design 4.4). See `tests/scripts/boss_v1.hot`, `boss_v2.hot`, `boss_rebase.hot` and `tests/engine/Damageable.h`.

- The shell derives from all bases: `struct Boss_Shell final : Actor, IDamageable, ITagged, hot::ShellBase`. The constructor arguments go to the primary base. An interface base needs a default constructor. In optimized AOT, the last base is `hot::ShellCore`.
- `libclang` reads the virtuals of each base. Each virtual gets one slot. The slots of the primary base come first, then the slots of each interface base in the order of the list. A name in two bases gets a suffix of its parameter types, for example `SLOT_Size_i`.
- `override` works for a virtual of any base. A virtual without an override calls the implementation of its base, for example `IDamageable::Armor`. `base.Armor()` calls the interface implementation when the primary base has no virtual with that name.
- The record keeps the list of the bases in `base_name` (`"Actor, IDamageable, ITagged"`). The shell hash uses the list. A reload that adds or removes a base is rejected (design 5.3: RECONSTRUCT): "native interface bases changed".
- Errors: the same signature in two bases; an override that matches `F()` of one base and `F() const` of another base; an override of a `final` virtual; a `final` interface class; a missing pure virtual override; a base named two times; a script class as interface base; interface bases on a class with a script parent (design 4.5: the shell of a child adds no overrides; put the base on the root); an interface base without a default constructor.
- Limits: a child of a script class cannot add interface bases. It inherits the interface bases of the root of its family.

## Translator session

`libclang` needs about 400 ms to parse the engine headers. `hotc::TranslatorSession` (`include/hot/hotc_lib.h`) keeps the result: the virtual methods, the destructor and the constructors of each native base class. `hotc::translate(options, session)` uses it. `hotc::translate(options)` makes a temporary session, so it parses each time.

- The key is the include flags, the current directory and the set of imports. A script that names a base class that the entry does not have causes one new parse. The next calls for both bases hit.
- The entry keeps the path, the change time (ns) and the size of each file that the parse included (`clang_getInclusions`, with the system headers). Each call checks them with `stat`. If one differs, the session parses again. The check costs about 1 ms.
- The stamp of each file also has the inode, the ctime and a hash of the content. An edit that restores the old time and keeps the size changes the ctime or the inode. Then the check reads the content and compares the hash. A header that changed in the last 2 seconds is always hashed. The same check is in the polling mode of `harness::FileWatcher` (the script file).
- The cache does not see a new header that comes earlier in the include path when no shadow stamp covers it. Use `TranslatorSession::clear()` for odd cases.
- Calls that use one session are thread safe. They wait for each other during a parse.
- `ClangFilesBackend` and `JitBackend` hold a session. `ClangFilesOptions::session` and `JitOptions::session` share one session between backends. `hotc` and `hotc` do one translate for each run, so they do not gain.

## `hotc` command line

The tool `hotc` compiles scripts ahead of time. This is not a backend. Any build can load the result. Call `hot::module_from_library(path)` and give the `Module` to the `Loader`. This call needs no backend object. The sample [samples/precompiled-library](https://github.com/rokups/hotpp/tree/master/samples/precompiled-library) is a complete project.

    hotc player_v1.hot --gen 1 -I include -I tests/engine --out-dir out/
    hotc player_v2.hot --gen 2 -I include -I tests/engine --out-dir out/

The tool `hotc` has two modes. The default mode builds a `.so`. The mode `--emit-cpp` only translates (see "Translator").

Command line of the build mode:

    hotc <script>... --gen N (--out-dir DIR | -o x.so) [-I dir]... [--resource-dir dir]
         [--cxx command] [--cxxflag flag]... [--slots file] [--no-strict-borrowed]

Command line of the translate mode. Without `-o`, the tool prints the C++ to the standard output:

    hotc --emit-cpp <script> --gen N [-o out.cpp] [-I dir]... [--prelude file]...
         [--slots file] [--resource-dir dir] [--no-strict-borrowed]

- `--raw-diagnostics` prints the compiler text as it is. By default `hotc` rewrites it (see "Diagnostics and debugging").
- `--no-strict-borrowed` makes the borrowed-storage findings (a stored `T*`, `T&` or `&x`) warnings. By default they are errors.
- The order of the classes in a script does not matter. A cycle of classes is an error.
- `<script>...` is one or more script files. Each script gives one library.
- `--gen N` is the generation number (1 or more).
- `-o x.so` builds one script into the library `x.so`. The C++ file goes to the directory of `x.so`. Use `-o` or `--out-dir`, not both.
- `--out-dir DIR` is the output directory. For a script `player_v1.hot`, `hotc` writes `DIR/player_v1.g1.cpp` and `DIR/player_v1.g1.so`.
- `-I dir` is an include dir. The translator and the compiler use it.
- `--resource-dir dir` is the Clang resource dir. The default is the one that the build found.
- `--cxx command` is the compiler command. The default is `clang++`.
- `--cxxflag flag` adds one flag to the compile command. Repeat it for more flags.
- `--slots file` is the slot file. The default is `DIR/slots.txt`. It keeps the slot order of the script methods between generations. Build the generations of one class in order.

The exit code is 0 for success, 1 for an error and 2 for a wrong command line. The tool prints the compiler errors at the script line. It does not need CMake.

Command line of the AOT mode. The tool translates generation 1 of each script. It does not compile:

    hotc --aot <script>... --out-dir DIR [-I dir]... [--prelude file]... [--no-strict-borrowed]
         [--format=std|fmt] [--resource-dir dir]

- For each script `hotc` writes `DIR/<name>.aot.cpp`. The register function has the name `hot_script_register_<name>`, so many scripts link into one program.
- `hotc` also writes `DIR/hot_aot_registry.cpp`. It defines the constant table `hot::aot_table` (`include/hot/aot.h`).
- The script names must be different. The class names must be different over all the scripts of a program. An AOT script has no slot file and no later generation.
