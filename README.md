# spp

Prototype of a hot-reload scripting runtime. This is step 3 (R0, R1 and F1) with the backend interface.
The design is in `scriptlang-design/scriptlang-design.md`.

## Layout

- `engine/` has the executable `minengine`. It has `Actor`, `World` and a small main program.
- `runtime/` has the script runtime: class records, shells, dispatch, reflection and the loader.
- `poc/r0/gen1.cpp` is hand-written code. The translator `spc` makes code with the same structure.
- `translator/spc_lib.cpp` is the translator library `spc_lib` (step F1). `translator/spc.cpp` is the thin file wrapper `spc`. `scripts/` has the example scripts.
- `runtime/backend.h`, `runtime/clang_files_backend.cpp`, `runtime/jit_backend.cpp` and `runtime/default_backend.cpp` are the script backends. `tools/sppc.cpp` is the tool `sppc`. `cmake/SppScripts.cmake` has `spp_add_scripts`.
- `tests/` has the GoogleTest cases (`r0_tests.cpp`, `r1_tests.cpp`, `backend_tests.cpp`, `translator_tests.cpp`), the F1 scenario `f1_scenario.h` and the shared header `test_support.h`.
- `cmake/CPM.cmake` fetches a dependency when the system has no copy of it.
- `poc/r1/` has generation 2 (`gen2.cpp`: new `Tick`, `hp` renamed to `health`, new `shield`), the rejected generation 4 (`gen4_base.cpp`: other shell layout) and `gen3_bad.cpp` (does not compile; not in the build).

## Build

    cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=clang++
    ninja -C build

For an AddressSanitizer build, use `-DCMAKE_CXX_FLAGS=-fsanitize=address` and the directory `build-asan`.

## Run

    build/minengine build/gen1.so

The command loads `gen1.so` and makes one `Player`. It ticks the world 3 times. It exits with 0 on success.

`poc/r1/make_bad.sh [build-dir]` compiles `gen3_bad.cpp` and shows that the compile fails. No library is made. The test `R1` uses the path of this missing library.

## Translator

    build/spc scripts/player_v1.script --gen 1 -o out.cpp -I engine -I runtime --slots player.slots

`spc` is a thin wrapper. It reads the files and calls `spc::translate` from `spc_lib` (`translator/spc_lib.h`). The function takes strings: script text, script name for `#line`, generation, slot file content, include dirs and resource dir. It returns strings: C++ text, new slot file content and diagnostics. It does not write a file. The translator reads the script and the imported headers. It asks libclang (C API) for the virtual methods of the native base class. A `spc::TranslatorSession` keeps the answer, see "Translator session". It writes the shell (one stub for each virtual), the state struct, the reflection table, the generation functions, the thunks and the record. It writes `#line` directives, so a Clang error points to the script line. It reports a missing pure virtual override and an override of a `final` method.

The script subset: `import`, `class N : Base`, fields with `@was("old")`, `override`, `fn`, `let`, `const`, `this.`, `super.`, `null`, float literals, `Math.Min` as `Math::Min`, `if`, `while`, `for`, `return`. The body rewrite works on tokens. The translator does not parse expressions. A pointer needs `->` in a body, except a field or a parameter that has a pointer type.

The slot file (`--slots`) keeps the slot order of the `fn` methods stable between generations. A generation can add a `fn`, but it cannot remove one.

### Translator session

libclang needs about 400 ms to parse the engine headers. `spc::TranslatorSession` (`translator/spc_lib.h`) keeps the result: the virtual methods, the destructor and the constructors of each native base class. `spc::translate(options, session)` uses it. `spc::translate(options)` makes a temporary session, so it parses each time.

- The key is the include flags, the current directory and the set of imports. A script that names a base class that the entry does not have causes one new parse. The next calls for both bases hit.
- The entry keeps the path, the change time (ns) and the size of each file that the parse included (`clang_getInclusions`, with the system headers). Each call checks them with `stat`. If one differs, the session parses again. The check costs about 1 ms.
- The cache does not see a new header that comes earlier in the include path, or a change that keeps the same time and size. Use `TranslatorSession::clear()` for these cases.
- Calls that use one session are thread safe. They wait for each other during a parse.
- `ClangFilesBackend` and `JitBackend` hold a session. `ClangFilesOptions::session` and `JitOptions::session` share one session between backends. `spc` and `sppc` do one translate for each run, so they do not gain.

## Backends

A backend turns script source into a loadable `Module`. A `Module` has the `script_register` function and an owner of the code memory (for a `.so`: a `dlopen` handle). The `Loader` takes a `Module` in `load` and in `request_reload`. The class `Backend` (`runtime/backend.h`) has the method `load(source, generation)`. It returns a `Module` or an error text with the compiler diagnostics.

The CMake cache option `SPP_BACKEND` selects the backend at build time. `make_default_backend()` returns it.

- `CLANG_FILES` (default). `ClangFilesBackend` calls `spc::translate` in the process, writes the `.cpp` in a work directory, runs an external `clang++` to make a `.so`, and loads it. The options are the compiler command, the include dirs, the resource dir, the extra flags and the work directory.
- `JIT`. `JitBackend` calls `spc::translate` in the process, compiles the C++ text in memory with the Clang library and loads the code with LLVM ORC. See the section "JIT backend".

Example: `cmake -S . -B build -DSPP_BACKEND=CLANG_FILES`.

## JIT backend

`cmake -S . -B build-jit -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DSPP_BACKEND=JIT`. LLVM and Clang (23.1.3 here) must be installed. CMake uses `find_package(LLVM)` and `find_package(Clang)` and links `clang-cpp` and `LLVM`. CPM does not fetch them.

How it works (`runtime/jit_backend.cpp`):

1. `spc::translate` makes the C++ text. The slot file text stays in memory in the backend object.
2. `clang::createInvocation` builds the cc1 command through the driver (so the libstdc++ include paths are right). The flags are `-std=c++20 -O1 -fPIC -gline-tables-only`, the include dirs, the resource dir and the extra flags. The input file exists only in an `InMemoryFileSystem` that overlays the real file system. `#line` makes the diagnostics name the script.
3. `EmitLLVMOnlyAction` gives an `llvm::Module`. The diagnostics go to a string.
4. One `LLJIT` (JITLink) lives for the session. Each `load` makes a new `JITDylib` and a `ResourceTracker`, adds the module, runs the static initializers (`LLJIT::initialize`) and looks up `script_register`. The Main JITDylib has a `DynamicLibrarySearchGenerator` for the process. The executable needs `ENABLE_EXPORTS` (`-rdynamic`), and it must link the runtime objects that the script code uses.
5. The `keep_alive` of the `Module` owns the generation. When the last copy goes, it runs `LLJIT::deinitialize`, `ResourceTracker::remove` and `removeJITDylib`. It also holds the `LLJIT`, so a `Module` can outlive the backend. As in the design, ORC does not track calls in flight. The safety rules are the same as for `dlclose`.

No file is written and no process starts. `tests/jit_tests.cpp` checks this: it sets the current directory and `TMPDIR` to a new empty directory, loads two generations and a bad script, and asserts that the directory is empty and that no child process ran. `strace -f` on the same test shows one `execve` (the test itself), only thread `clone3` calls and no file opened for writing. The same file tests exceptions (a `throw` and a `catch` in JIT code, and a `throw` that goes through JIT frames to the host), `-include-pch`, and the life of a generation.

Facts that differ from the first design:

- `-g` leaks. After a failed compile, Clang does not free the temporary debug metadata (about 270 KB for `bad.script`, found with LeakSanitizer). `-gline-tables-only` does not leak, and it costs the same time. `JitOptions::debug_flag` sets the flag. LLJIT does not register a debugger plugin, so full debug info has no use now.
- `CompilerInstance::ExecuteAction` prints "N errors generated." to stderr when `ShowCarets` is on. The backend uses its own copy of the diagnostic options for the printer, so the text goes only to the result.
- `CompilerInvocation` keeps pointers into the argument strings. The strings must live as long as the compile.
- `LLVMConfig.cmake` probes C headers, so CMake calls `enable_language(C)` for the JIT build.
- The global constructors of a script (the class record) are run by `LLJIT::initialize`. Without it, `Player_record` has no value.

### Measurements

Machine: 32 threads, Linux 7.2, clang 23.1.3, `-O0`/`-O1` as noted, the page cache is warm. Tool: `build/bench_backends` (`tools/bench_backends.cpp`, `std::chrono`). Times in ms for one `load` (translate + compile + load) of `player_v1.script` (v1) and `player_v2.script` (v2). "Cold" is the first load in a new process (a new session). "Warm" is the median of 5 later loads in the same process. Warm backends share one translator session, except in the rows "no cache" (`--no-cache`). Cold v1 always parses the headers. Cold v2 and all warm loads use the cache, except v1 in the rows "no cache".

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

The tool `sppc` compiles scripts ahead of time. This is not a backend. Any build can load the result. Call `script::module_from_library(path)` and give the `Module` to the `Loader`. This call needs no backend object.

    sppc player_v1.script --gen 1 -I engine -I runtime --out-dir out/
    sppc player_v2.script --gen 2 -I engine -I runtime --out-dir out/

Command line:

    sppc <script>... --gen N --out-dir DIR [-I dir]... [--resource-dir dir]
         [--cxx command] [--cxxflag flag]... [--slots file]

- `<script>...` is one or more script files. Each script gives one library.
- `--gen N` is the generation number (1 or more).
- `--out-dir DIR` is the output directory. For a script `player_v1.script`, `sppc` writes `DIR/player_v1.g1.cpp` and `DIR/player_v1.g1.so`.
- `-I dir` is an include dir. The translator and the compiler use it.
- `--resource-dir dir` is the Clang resource dir. The default is the one that the build found.
- `--cxx command` is the compiler command. The default is `clang++`.
- `--cxxflag flag` adds one flag to the compile command. Repeat it for more flags.
- `--slots file` is the slot file. The default is `DIR/slots.txt`. It keeps the slot order of the `fn` methods between generations. Build the generations of one class in order.

The exit code is 0 for success, 1 for an error and 2 for a wrong command line. The tool prints the compiler errors at the script line. It does not need CMake.

The CMake function `spp_add_scripts` (`cmake/SppScripts.cmake`) only runs `sppc`:

    spp_add_scripts(TARGET pre_gen1 GEN 1 SCRIPTS scripts/player_v1.script
                    INCLUDES engine runtime FLAGS ${flags} DEPENDS ${headers})
    spp_add_scripts(TARGET pre_gen2 GEN 2 SCRIPTS scripts/player_v2.script AFTER pre_gen1 ...)

`TARGET` is a custom target that makes the libraries in `OUT_DIR` (default `${CMAKE_BINARY_DIR}/spp_scripts`). `AFTER` orders a generation after the earlier one.

## Test

CMake uses CPM. It takes GoogleTest from the system, and fetches it only when it is missing. libclang must come from the system. The JIT build also needs the LLVM and Clang development packages.


    ninja -C build && ctest --test-dir build

All tests are GoogleTest cases. CMake registers them with `gtest_discover_tests`.

- `R0` (`tests/r0_tests.cpp`) loads `gen1.so` and makes one `Player`. It ticks the world 3 times and calls both `TakeDamage` overloads. It checks the type name, the position and the field `hp` by reflection.
- `R1` (`tests/r1_tests.cpp`) binds a delegate to `heal` through a thunk. It reloads gen 2 at the frame boundary and checks the address, the migrated state, the new `Tick` and the delegate. It then tries gen 4 and a missing library. Both must be rejected, and gen 2 must keep running.
- `BackendTests` (`tests/backend_tests.cpp`) use the backend that `SPP_BACKEND` selected. They run the F1 reload scenario (`tests/f1_scenario.h`): generation 1 then generation 2, the same address, `hp` becomes `health`, `shield` is 25 and a delegate calls the new `heal`. A bad script (`scripts/bad.script`) must give an error that names `bad.script` and the line. The runtime must keep running after the error.
- `TranslatorSession` (`tests/translator_tests.cpp`) checks the cache: a hit gives the same output as a miss, a changed header (also a nested one) invalidates it, a second base class works in the same session, and the second translate takes less than 150 ms (the real value is about 1.3 ms).
- `JitTests` (`tests/jit_tests.cpp`, only in a JIT build) check that no file appears and no child process runs. They also check exceptions in JIT code, a precompiled header and the life of a generation.
- `PrecompiledTests` load the libraries that `spp_add_scripts` made with `sppc`, through `module_from_library`. They run the same F1 scenario. They run for each `SPP_BACKEND` value. One test runs `sppc` on `bad.script` and checks the exit code and the error line.

## Reload

`Loader::request_reload` validates a `Module` with no side effect: `script_register`, base class, shell layout hash, slot count. `Loader::commit_pending` runs at the frame boundary (the `World` hook). It allocates all new state blocks, migrates, swaps the table and layout, sets `active_generation` and frees the old blocks.

## Not done yet

The `onReload` hook, the `convert` hook, generation unload, closures, string interpolation, script inheritance and constructors in scripts.
