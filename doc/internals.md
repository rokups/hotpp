---
layout: default
title: Runtime internals
nav_order: 46
---

Notes on the JIT backend, threads, closures and debug information. They help people who work on the runtime.

## JIT backend

LLVM and Clang must be installed. CMake uses `find_package(LLVM)` and `find_package(Clang)` and links `clang-cpp` and `LLVM`. CPM does not fetch them.

How it works (`src/runtime/jit_backend.cpp`):

1. `hotc::translate` makes the C++ text. The slot file text stays in memory in the backend object. A removed function keeps its slot as a defunct entry.
2. `clang::createInvocation` builds the `cc1` command through the driver (so the `libstdc++` include paths are right). The flags are `-std=c++20 -O1 -fPIC -gline-tables-only`, the include dirs, the resource dir and the extra flags. The input file exists only in an `InMemoryFileSystem` that overlays the real file system. `#line` makes the diagnostics name the script.
3. `EmitLLVMOnlyAction` gives an `llvm::Module`. The diagnostics go to a string.
4. One `LLJIT` (`JITLink`) lives for the session. Each `load` makes a new `JITDylib` and a `ResourceTracker`, adds the module, runs the static initializers (`LLJIT::initialize`) and looks up `script_register`. The Main `JITDylib` has a `DynamicLibrarySearchGenerator` for the process. The executable needs `ENABLE_EXPORTS` (`-rdynamic`), and it must link the runtime objects that the script code uses.
5. The `keep_alive` of the `Module` owns the generation. When the last copy goes, it runs `LLJIT::deinitialize`, `ResourceTracker::remove` and `removeJITDylib`. It also holds the `LLJIT`, so a `Module` can outlive the backend. As in the design, ORC does not track calls in flight. The safety rules are the same as for `dlclose`.

No file is written and no process starts. `tests/integration/backends/jit_backend_tests.cpp` checks this: it sets the current directory and `TMPDIR` to a new empty directory, loads two generations and a bad script, and asserts that the directory is empty and that no child process ran. `strace -f` on the same test shows one `execve` (the test itself), only thread `clone3` calls and no file opened for writing. The same file tests exceptions (a `throw` and a `catch` in JIT code, and a `throw` that goes through JIT frames to the host), `-include-pch`, and the life of a generation.

Facts that differ from the first design:

- `-g` leaks. After a failed compile, Clang does not free the temporary debug metadata (about 270 KB for `bad.hot`, found with LeakSanitizer). `-gline-tables-only` does not leak, and it costs the same time. `JitOptions::debug_flag` sets the flag. `LLJIT` does not register a debugger plugin, so full debug info has no use now.
- `CompilerInstance::ExecuteAction` prints "`N errors generated.`" to `stderr` when `ShowCarets` is on. The backend uses its own copy of the diagnostic options for the printer, so the text goes only to the result.
- `CompilerInvocation` keeps pointers into the argument strings. The strings must live as long as the compile.
- `LLVMConfig.cmake` probes C headers, so CMake calls `enable_language(C)` for the JIT build.
- The global constructors of a script (the class record) are run by `LLJIT::initialize`. Without it, `Player_record` has no value.

### Load times

Machine: 32 threads, Linux 7.2, clang 23.1.3, `-O0`/`-O1` as noted, the page cache is warm. Tool: `bench_backends` (`tools/bench/bench_backends.cpp`, `std::chrono`). The rows below come from an earlier version of the tool, which had a fixed backend, the options `--pch` and `--make-pch` and `-O1` for the scripts. The current tool tests all backends in one run (see "Run-time cost" below). Times in ms for one `load` (translate + compile + load) of `player_v1.hot` (v1) and `player_v2.hot` (v2). "Cold" is the first load in a new process (a new session). "Warm" is the median of 5 later loads in the same process. Warm backends share one translator session, except in the rows "no cache" (`--no-cache`). Cold v1 always parses the headers. Cold v2 and all warm loads use the cache, except v1 in the rows "no cache".

| Configuration | v1 cold | v2 cold | v1 warm | v2 warm |
|---|---|---|---|---|
| JIT, `-O1` (default), cache | 1306 | 806 | 812 | 808 |
| JIT, `-O1`, no cache | 1295 | 812 | 1220 | 809 |
| JIT, `-O0`, cache | 1283 | 781 | 786 | 789 |
| JIT, `-O1`, `-include-pch`, cache | 622 | 166 | 170 | 166 |
| JIT, `-O1`, `-include-pch`, no cache | 637 | 166 | 586 | 169 |
| `CLANG_FILES`, `-O0 -g` (default), cache | 1370 | 908 | 913 | 907 |
| `CLANG_FILES`, `-O1`, cache | 1394 | 941 | 957 | 947 |
| `CLANG_FILES`, `-O1`, no cache | 1395 | 940 | 1356 | 940 |

`CLANG_FILES` with a PCH (P1, S19). The earlier tool made a PCH only in the JIT backend (`--make-pch`). The current tool has no PCH option, but `--cxxflag` still passes `-include-pch`. For `CLANG_FILES`, make the PCH with the compiler and pass it through `--cxxflag`.

    clang++ -std=c++20 -fPIC -g -O1 -resource-dir=$(clang++ -print-resource-dir) -I include -I tests/engine \
        -x c++-header pch.hpp -o p.pch
    bench_backends --warm 9 --cxxflag -O1 --cxxflag -include-pch --cxxflag $PWD/p.pch

| Configuration (S19 run, same machine, other load) | v1 cold | v2 cold | v1 warm | v2 warm |
|---|---|---|---|---|
| `CLANG_FILES`, `-O1`, cache | 1693 | 1111 | 1110 | 1104 |
| `CLANG_FILES`, `-O1`, `-include-pch`, cache | 931 | 344 | 357 | 349 |

The PCH takes 1.1 s to make, once, and is 13.5 MB. It saves 755 ms of 1104 ms in a warm load (68%). The numbers of this run are higher than the rows above, because the machine had more load. Compare the two rows of one run only.

P1 criterion (design 8.2): under 2 s for one module after a one-line change, fail over 5 s. Result: pass in every configuration, also `CLANG_FILES` with no PCH (1.1 s). With a PCH: JIT 166 ms and `CLANG_FILES` 350 ms. The test headers are small. Measure again with the real engine headers.

Before the cache, the translate time was 411 ms in each load. The split of one translate (v2, `std::chrono` in `translate_impl`, measured before the change) was: `libclang` parse of the imports 406 ms, script parser 0.4 ms, visit of the base class 0.5 ms, text emit 2.7 ms. Only the `libclang` parse is large.

Translate alone (the bench prints it): 406 ms with a new session (miss), 1.3 ms with the same session (hit). The target was 50 ms.

Phases of one JIT load (v2, warm, cache): translate 1.4, compile 782, ORC (code generation, link, initializers, lookup) 19. With `-include-pch`: translate 1.4, compile 146, ORC 18. A new session parses the headers again, so v1 warm in the rows "no cache" costs 411 ms more.

- JIT against `CLANG_FILES` with the same `-O1`: about 140 ms less (15%). The saving is the process start and the link step. The compile itself is the main cost in both.
- `-include-pch` works with the in-memory compile. `JitBackend::make_pch(header, out)` makes the PCH in the process (886 ms once, 10 MB). The header must start with the same includes as the generated file. It saves 636 ms of 782 ms in the compile phase (81%) and 642 ms of 808 ms in a load (79%, with the translator cache). The PCH keeps the path and the time stamp of the header and needs the same flags (for example `-fsanitize`).
- Size: `backend_tests` is 1.29 MB (`CLANG_FILES`) and 1.69 MB (JIT), so +0.4 MB. Both builds already need `libclang.so` (45 MB) and `libLLVM.so` (173 MB) for the translator. The JIT build also needs `libclang-cpp.so` (78 MB). Peak RSS of the bench process: 100 MB (`CLANG_FILES`), 299 MB (JIT), 246 MB (JIT with PCH).

## Threads and the safe point

The tests are in `tests/integration/threading/parallel_tick_tests.cpp` (suite `ParallelTick`).

`World::TickParallel(dt, threads)` ticks the actors on worker threads. The workers start once. They stop when the world ends. Each worker ticks one chunk of the actor list. The main thread waits at a barrier. After all workers reach the barrier, the main thread runs the frame boundary hook. The workers stay idle until the hook ends.

The reload commit runs only in this hook. At that time no thread is in script code. The barrier also orders the memory, so the workers see the result of the commit.

A compile thread can call `Loader::request_reload` and `Loader::reload_pending` at any time. These functions take the mutex of the loader. They only validate. They change no live class and no live object. `commit_pending` takes the same mutex.

The debug check is the counter `hot::in_script_tick()`. The engine wraps each actor tick in a `hot::ScriptTickScope`. `commit_pending` asserts that the counter is zero. It also counts a violation in `hot::unsafe_commit_count()`. The check exists in debug builds only. With `NDEBUG`, `hot::kTickChecks` is false, `ScriptTickScope` does nothing and the commit has no check.

A class that a new generation drops is removed (design 6.1). Its record stays. Its live objects keep their state. Each slot points to `hot::StateBase::__removed`, and the slot has the key `kRemovedSlotKey` ("!removed"). No signature matches that key, so a call detaches: it does not reach the slot and gives a default result. The runtime logs the first call and counts all calls. A removed function in a live class keeps its slot as a defunct entry that also points to `StateBase::__removed` (an exported member: the generated slot table names it). The slot has the key `kDefunctSlotKey` ("!defunct"). A call of it throws `hot::ScriptError` "call of removed script function Cls::fn" (`removed_function_called`). The name comes from `ClassRecord::slot_names`, which the translator emits for a class with defunct slots. The slot stays defunct until a full restart. The class can come back in a later generation. `Loader::stats()` and `Loader::stats_text()` (the `script_stats` text) list the generations, the pins, the live objects, the classes with their slots and the removed classes.

The sample `samples/hot-reload` shows the reload path. The test host `tests/engine/Harness` ticks a world and reloads a script when the file changes (`FileWatcher`: `inotify` on Linux, polling else). The tests in `tests/integration/reload/` (`integration_reload_tests`) test this.

The warnings of a reload use readable names: C++ type names (`int`, `std::string`) and class names (`Hero`, not `HeroState_g2`).

Rules for threads:

- A worker reads class records and state blocks. It writes the state of its own actors only.
- A worker does not call the loader.
- Any thread can create and destroy script objects when the build has `HOT_THREAD_SAFE=ON` (the default). `new` in script code, `ClassEntry::create` and the release of the last handle of an object are all valid on any thread. The list of live objects (`LiveObjects`, `src/runtime/state.cpp`) has a small spinlock for this. It is held for a few stores. It costs 5 to 17 ns for each `new` and free, depending on the work around it (see [Benchmarks](benchmarks.html#the-cost-of-new)). An object can be destroyed on another thread than the thread that made it.
- With `HOT_THREAD_SAFE=OFF` the list has no lock, and only one thread at a time creates or destroys script objects. A host that does this on more than one thread must serialize it with its own lock. A debug build asserts when two threads change the list at the same time.
- `Loader::stats`, `frame_boundary` and `reset` read the list under the lock. They do not race with a thread that creates or destroys a script object: the count of objects is a snapshot. The other rules do not change: call `frame_boundary` at the safe point, because it unloads code.
- `commit_pending` does not run while another thread creates or destroys a script object. The commit migrates the objects, and the migration runs script code, so it cannot hold the lock. A thread that destroys an object during the migration is a use-after-free. In `StopTheWorld` mode the host must make sure of this: run the commit at the safe point, when no thread is in script code or in `new`. In `EntryCounters` mode each creation and destruction is a script entry, so the commit blocks them.
- The host and the runtime library must use the same `HOT_THREAD_SAFE` value. `Loader` throws `std::runtime_error` when they differ. The backends compile the scripts with the value of the runtime (`-DHOT_THREAD_SAFE=0` when it is off). If you compile a script with your own flags, pass the same value.
- The main thread calls `load`, `commit_pending`, `frame_boundary` and `stats`.
- Any thread can pin or unpin a generation.

Test results: `R5_CommitOnlyAtBarrier` runs 20 seeds with 1000 actors and 2 workers. `R5_RejectedReloadFromOtherThread` runs 5 seeds. Both tests pass in the normal, ASan and TSan builds. They also pass with `--gtest_repeat=20` in the normal and TSan builds. TSan reports no race. When the lock in `request_reload` is removed, TSan reports a race. So the test can find this kind of error.

### Entry counters: a commit without a frame boundary

Some engines have no frame boundary. For them, the loader has a second mode (design 6.3):

```cpp
loader.set_sync_mode(hot::SyncMode::EntryCounters);  // default: hot::SyncMode::StopTheWorld
```

The mode applies to the whole process. Set it when no thread is in script code. The destructor of the `Loader` sets `StopTheWorld` again.

How it works:

- Each script entry is a `hot::ScriptEntry`. The entries are the slot calls (`hot::call`: shell stubs, thunks and script virtual calls), the closure calls and the script destructors.
- The outermost entry of a thread increments one global count. A nested entry (script, then native, then script) increments only a thread-local depth. So a nested entry never blocks.
- `commit_pending` opens a `hot::CommitWindow`. It closes the gate and waits until the count is zero. A new outermost entry waits at the closed gate. Then the commit runs. After the commit, the gate opens and the waiting threads continue in the new generation.
- The commit thread can run script code in the window, for example an `onReload` hook.
- `commit_pending` can run on any one thread. It fails with an error when the calling thread is in script code. `frame_boundary` can run on the commit thread after the commit.

Cost: in the default mode, each entry reads one global flag (a relaxed atomic load) and takes no branch. Define `HOT_NO_ENTRY_COUNTERS` to remove the check at compile time. In the `EntryCounters` mode, each outermost entry does one atomic increment and one atomic decrement of a shared counter.

Limits:

- A deadlock occurs when script code waits for a thread that waits at the gate, or for the end of the commit.
- The creation of a script object through the factory (`ClassEntry::create`) is an entry (S27), so a commit cannot swap the class during the construction. Native code that reads the fields of an object holds a `hot::ScriptEntry`. `new` in script code runs inside a script entry. With `HOT_THREAD_SAFE=OFF`, object creation and destruction stay on one thread at a time.
- A thread that entered script code in the default mode is not counted.
- `ScriptTickScope` and its debug check apply to the default mode only. A worker can wait at the gate inside a `ScriptTickScope`.

Diagnostics: `hot::active_entries()`, `hot::entry_depth()`, `hot::commit_window_closed()`, `hot::commit_window_count()` and `hot::blocked_entry_count()`.

The tests are in `tests/unit/runtime/entry_counter_tests.cpp` and `tests/integration/threading/entry_counter_tests.cpp` (suite `EntryCounters`). In the integration test, 4 worker threads call `Tick` on 64 actors in a loop. Another thread commits 8 reloads at the same time. Each `Tick` calls the native `Util::Probe`, and the probe calls the script `TakeDamage`. The test checks that no call is lost, that no call runs on a half migrated object, and that a nested call during a waiting commit does not deadlock. The unit test opens 2000 commit windows while 4 threads enter.

## Closures and unload

An arrow lambda becomes `hot::make_closure(GEN, "Class::fn lambda", [=](params) { ... })`. It captures by value. `this` is the pointer `__self`. The body reads the state pointer again, so a lambda never keeps a pointer to a state block. The result is a `hot::Closure<Sig>`. It holds one pin of its generation (`hot::pin`, `unpin`, `pin_count`; the counts are atomic). Each call checks the generation. When the generation is not live, the call logs a stale call, counts it (`hot::stale_call_count()`) and returns a default value. The body does not run. A `Closure` converts to `std::function` and to an engine delegate that has `Bind(std::function<Sig>)`. The `std::function` holds the guarded lambda (`hot::detail::GuardedFn`: the pin, the guard and the lambda), not the `Closure`. So a call through it is one indirect call.

`World::AddTimer(seconds, fn)` stores a one-shot callable. `World::Tick` fires it before the frame boundary hook and then destroys it. `ClearTimers()` drops all timers.

The `Loader` owns all modules. A reload retires the old generation. `Loader::frame_boundary()` (call it in the hook, after `commit_pending`) unloads a retired generation when all of these are true: the pin count is zero, a frame boundary passed after the commit, and `code_refs` is zero. The unload is `dlclose` (`CLANG_FILES`) or the removal of the ORC resource tracker (JIT). `Loader::stats()` gives the active generation, each generation with its state (active, retired, unloaded), its pins and its `code_refs`, and the number of live objects.

Result of R3: the stale call is logged once and the old body does not run. No ASan error. Result of R6: generation 2 of the chain 1, 2, 3 unloads after its closure is dropped, and later ticks are clean. Generation 1 unloads too (S10). A core module holds the class record, the factory, the thunks and the vtables. The core is a second instance of the same generation code, and it never unloads. Destroy the last copy of a closure on the main thread. The destructor code is in the module.

## Debug information

Diagnostics:

- `hotc::rewrite_diagnostic(text, scripts, generated)` is a pure function in the translator library. It changes the Clang text to script names (`Player_Shell` to `Player`, `Player__heal` to `Player.heal`, `int32_t` to `int`). It does not count the hidden `__self` argument. It gives a plain message for the common mistakes (the table is in `src/translator/diag_rewrite.cpp`). It shows the script line in place of the generated line.
- Both backends and `hotc` use it. `raw_diagnostics` in the backend options and `hotc --raw-diagnostics` keep the compiler text.
- `#line` has no column in standard C++, so the generated C++ keeps `#line N "file"` only. The rewriter takes an optional `SourceMap` (the backends pass `TranslateResult::source_map`). It finds the generated line from the `#line` directives, maps the Clang column through the map, and prints the real script column. If the map has no segment for the column, the column is 1 for a `.hot` script. Without a map the column is 1 for a `.hot` script.
- A `.cpp` script has its own rule. The translator copies a body as it is, so the Clang column is right when the first col-1 characters of the generated line and of the script line are equal. Then the column stays, and the caret and fix-it lines stay when the two lines are equal. When hotc rewrote the line, for example a lambda capture `[this]`, the column is 1, the script line shows and the caret is dropped. An error in generated glue maps to the nearest script line with column 1.
- `hotc::rewrite_translator_diagnostic(text, scripts)` gives the translator errors (parse, check, libclang facts) the same final form: `script:line:col: error: message`, the script line in the gutter form, and the count line (`1 error generated.`). `hotc` and both backends use it. `--raw-diagnostics` keeps the plain translator text.
- The backends compile with `-Werror=return-type`. A missing `return` is an error.
- The rewriter selects the dialect for each file. A file name that ends in `.cpp` gets the C++ dialect. Other files get the `.hot` dialect. See [Diagnostics of a .cpp script](diagnostics.html#diagnostics-of-a-cpp-script).
- `last_script_line` finds the script line for an error in generated glue. It skips the `#line` directives that name a generated file. It does not skip every `.cpp` name: a `.cpp` name that is a script counts.
- The translator errors do not pass through `rewrite_diagnostic`. `rewrite_translator_diagnostic` formats them.
- The golden cases are in `tests/golden/diagnostics/` (75 cases). The cases 74 to 77 are `.cpp` cases: the case file is `NN_name.cpp` and not `.hot`. The test is `DiagnosticMistakes`.

Debug information:

- The generated C++ has `#line` directives. The JIT and `hotc` use `-gline-tables-only`, so the DWARF line table names the `.hot` file. The tests `DebugInfo.*` check this with `llvm-dwarfdump`.
- The JIT backend registers each object with the debugger through the GDB JIT interface (`llvm::orc::enableDebuggerSupport`). `JitOptions::debugger_support` turns it off. `debugger_support()` and `debugger_error()` tell the result. The test `JitTests.DebuggerGetsObjectWithScriptLines` reads `__jit_debug_descriptor`. LLVM 23 has no `createJITLoaderGDBRegistrar`.
- `tools/lldb/hot_formatters.py` and `tools/lldb/hot.lldbinit` show state blocks, shells and handles by script name. The command `hot-bt` prints the script frames. A synthetic child provider hides `__hot_owner` from the state blocks. `tools/natvis/hot.natvis` does the same for Visual Studio.
- The pure Python helpers have a test (`PythonLldbFormatters`, `python3 -I`). `lldb` itself, a real debugger session and Visual Studio were NOT run. The F3 check (breakpoint in `heal`, step, inspect `this.health`) stays open.

### What the debugger shows

What the debugger shows:

- Each script class has a state struct, for example `PlayerState_g1`. Its first member is `hot::ShellBase* __hot_owner` (offset 0, with a `static_assert`).
- Each shell derives from `hot::ShellBase`, which derives from `hot::ShellCore`. `ShellCore` holds only the class record (`cls`). `ShellBase` adds the state pointer and the list of live objects. A shell of optimized AOT derives from `ShellCore` only.
- Script methods are members of the state struct. A frame shows `this` (the script fields) and the parameters. The locals `__self` and `__st` have the attribute `HOT_NODEBUG_LOCAL` (`[[gnu::nodebug]]` on Clang and GCC).
- A slot holds a pointer to the state member (`hot::SlotFn`, for example `PlayerState_g1::Tick`). `hot::call<Sig>(__self, slot, args...)` calls the member. No `Player__Tick` wrapper exists. A script function, a constructor and a destructor keep a hidden `HOT_HIDDEN` wrapper for direct script calls.
- The member prologue has the stale-generation check. A parameter with a body type has the name `__<name>` in the member. The first body line copies it to `<name>`.
- A line breakpoint on a `fn` line hits once. Step-in from the engine reaches the member after the frame of the shell override. The shell override maps to the class line.
- Every generated line that makes code has a `#line N "script.hot"` before it. Breakpoints and stepping use the `.hot` lines.
- A name that starts with `__` is internal state. A script field with such a name is a compile error.

For runtime header authors:

- `include/hot/runtime/hidden.h` has two macros. `HOT_HIDDEN` is `always_inline` and `nodebug`: the debugger never sees the function. `HOT_NODEBUG` is `nodebug`: the debugger steps over the function.
- Put one of them on each runtime helper, so that step-in does not enter it.
- Clang rejects `#pragma clang attribute` with `nodebug`. The C++ standard library functions (`c_str`, `shared_ptr`, `make_shared`) still show on step-in at `-O0`.

Editors:

- `editors/vscode/package.json` has `contributes.breakpoints` for the language `hot`. The C++ extension (`cppdbg` with `gdb`, or `lldb`) works with no custom launch configuration.
- Option: the `gdb` setting `skip -gfi "*/include/c++/*"` hides the `libstdc++` stops.
- `tools/natjmc/hot.natjmc` is a Just My Code file. It marks `hot::*`, the shell, the thunks, the virtual helpers, the base forwarders and `state_of` as non-user code. Status: unverified. Visual Studio reads it from the user Visual Studio Visualizers folder. `cppvsdbg` in VS Code may not read it.
- The CI builds and tests the MSVC code paths (the job `windows-msvc`). A real Visual Studio debugger session is untested.
