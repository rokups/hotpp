---
layout: default
title: Benchmarks
nav_order: 43
---

The tool `bench_backends` measures the run-time cost of script code. Each case has up to three versions that do the same work:

- a `.hot` script,
- a [`.cpp` script](cpp-scripts.html): a plain C++ file that `hotc` translates (the `libclang` frontend),
- a native C++ twin.

The native twin is the reference. The result is a ratio: script time divided by native time.

A second tool, `bench_backends_opt`, runs the same cases with the scripts built as [optimized AOT code](embedding-backends.html#optimized-aot) (`MODE AOT OPTIMIZED`). It calls this backend `aot-opt`.

Every section of the tool is a matrix. The two script modes (`.hot` and `.cpp`) run in each backend (`files`, `jit` and `aot`). This is true for the cases, for `new`, for reloadable access, for the reload and for the reload chain.

## Summary

All values come from the `Results` section below. They are the median of 3 runs.

- A loop of fields, numbers, arrays, module variables and calls inside one script runs at native speed (0.94x to 1.0x). This is true for `.hot` and for `.cpp`, in the three backends and in `aot-opt`.
- A call from the host into a script costs 2.2x to 2.4x of a native virtual call in `files` and `aot`. This is 1.8 to 2.1 ns more. In `jit` it costs 2.9x (about 2.8 ns more). `.hot` and `.cpp` are equal. See [The cost of a call](#the-cost-of-a-call).
- A call after 0, 1, 10 or 100 reloads costs the same: 3.4 ns (2.3x) in `files` and 4.3 ns (2.9x) in `jit`, for `.hot` and for `.cpp`.
- A script to native call runs at native speed (1.46 ns) in the three backends.
- `new` and free of a script object cost 47 to 50 ns in `.hot`: 4.4x to 4.6x of `std::make_shared`. In `.cpp` with a raw pointer, they cost 35 to 37 ns: 3.2x to 3.4x.
- A script to script virtual call costs 0.35 to 0.9 ns more than a native one. This is true for `.hot` and `.cpp`.
- A `.hot` closure call costs 1.5x. A `.hot` `std.vector` index costs 1.4x. The `.cpp` twins of both run at native speed.
- A reload of 100 objects with 65 fields takes 0.10 ms in `files` and 1.4 ms in `jit`. The compile is not in it.
- Optimized AOT code (`aot-opt`) removes the cost of the slot. A call from the host into a script costs 1.46 ns, against 3.51 ns in `aot`: native speed (1.0x). A native to script virtual call costs 1.46 ns, against 3.80 ns. A script to script virtual call costs 1.75 ns in `.hot` and 1.46 ns in `.cpp`, against 2.63 ns and 2.35 ns in `aot`. See [Optimized AOT](#optimized-aot-aot-opt).
- `new` and free in `aot-opt` cost 34.1 ns in `.hot` and 8.2 ns in `.cpp`, against 59.4 ns and 38.8 ns in `aot`. The fields are members of the shell, so an object is one allocation. See [The cost of `new`](#the-cost-of-new).
- The `aot` and `aot-opt` values in these lines come from one build with `HOT_THREAD_SAFE=ON` (see [Machine](#machine)).

## How to run

Build the tool in a Release build. A build with no `CMAKE_BUILD_TYPE` compiles the runtime without optimization, and the ratios are larger. The tool is part of the build when `HOT_BUILD_TESTS` is `ON`.

    cmake -S . -B build-rel -G Ninja -DCMAKE_BUILD_TYPE=Release -DHOT_BUILD_TESTS=ON
    cmake --build build-rel --target bench_backends
    build-rel/bin/bench_backends --no-load

One binary tests every backend of the build:

- `files`: the clang files backend compiles each script at run time (`-O2`).
- `jit`: the JIT backend compiles each script in the process (`-O2`).
- `aot`: the scripts are C++ in the binary (`MODE AOT_RELOAD` of `hot_add_host`, `-O2`).

The program `bench_backends_opt` is a second executable with the same sources. It builds the scripts as optimized AOT code (`hotc --aot --aot-optimized`, `-O2`) and tests only that code. One program cannot hold two builds of the same scripts, so this needs its own program. Its backend is `aot-opt`, and `--backend all` runs only `aot-opt`. The loader refuses to reload optimized code, so the reload cases do not run there.

    cmake --build build-rel --target bench_backends_opt
    build-rel/bin/bench_backends_opt --no-load

The native twins also use `-O2`. A backend that the build does not have is skipped. The `.hot` scripts and the `.cpp` scripts are built in the same way in each backend.

The table has a row for each case, for the reload and for each step of the chain. It has two columns for each backend, `hot` and `cpp`. Each column has a time and a ratio. The CSV file has the same rows and columns. The text `n/a` marks a cell that has no value (see [Marks](#marks)).

| Option | Effect |
|---|---|
| `--backend all\|files\|jit\|aot` | test one backend only (`bench_backends_opt`: `all\|aot-opt`) |
| `--filter text` | run only the cases whose name contains the text |
| `--reps N` | timed runs of each case; the result is the median (default 7) |
| `--iters N` | loop count of each case |
| `--reloads N` | the largest N of the reload chain (default 100) |
| `--csv file` | write the table also as CSV |
| `--no-load` | skip the load section (translate and compile time) |
| `--list` | print the backends and the cases, with the `.hot` and the `.cpp` file of each case |

The tool checks that each script gives the same result as its twin. A `!` after a time marks a different result. No `!` appeared in the runs of this page.

Run the tool alone, on a quiet machine. Other work on the machine changes the numbers. One run of all backends takes about 8 minutes: about 250 s in `files`, 220 s in `jit` and 4 s in `aot` (also 4 s in `aot-opt`). Each case and each reload needs one compile in `files` and in `jit`. Pin the process to one core to get less noise: `taskset -c 6 build-rel/bin/bench_backends --no-load`.

## The three versions of a case

The scripts are in `tools/bench/scripts`. A `.hot` script has the name of the case, for example `fields.hot`. Its `.cpp` twin has the prefix `cpp_`, for example `cpp_fields.cpp`. Both derive from the same native base class and run the same loop. The checked result is the same.

A `.cpp` script is plain C++. Its arrays and its `std::vector` have no bounds check, as in any C++ code. A `.hot` array and a `.hot` `std.vector` have a check when `HOT_RUNTIME_CHECKS` is `ON`.

A `.cpp` script has no script class handles. The `.cpp` twins of the cases with handles use raw pointers to script objects (see [Script classes as types](cpp-types.html#script-classes-as-types)). A raw pointer has no null check and no generation check. The null check of the `handles` case stays in the code. Four twins measure the same work as the `.hot` script: `alloc`, `state_field`, `handle_call` and `slot_script`.

The `reload` case and the `chain` case have a `.cpp` version too. The tool makes its text. It is the same class in C++.

### Marks

| Mark | Row | Meaning |
|---|---|---|
| `.cpp` `n/a` | handle dereference and null check | A `.cpp` script has no handles. The row below it, the pointer chain, is the `.cpp` case. It has its own native twin with raw pointers. |
| `.hot` `n/a` | pointer dereference and null check | A `.hot` script has no raw pointers. |
| `.hot` `n/a` | module function and static method | A `.hot` free function and static method are not measured yet. The native twin is a free function and a static method. |
| `aot` `n/a` | reload and reload chain | The tool loads the AOT classes with `Loader::load_aot`. It does not reload them. |
| `aot-opt` `n/a` | reload and reload chain | Optimized AOT code has no reload. |
| `-` | native owners of `new` | The row is a native twin only. It has no script. |

## Machine

The CPU is an AMD Ryzen 9 3950X (a desktop CPU, 16 cores, 32 threads). The software is Linux 7.2, glibc 2.44, clang 23.1.3 and `libstdc++`. The build uses `CMAKE_BUILD_TYPE=Release`, `HOT_RUNTIME_CHECKS=ON` and `HOT_TRACEBACK=OFF`. The process runs on one core (`taskset -c 6`). The values are the median of 3 runs of the tool (each run gives the median of 7 timed runs).

The call rows have newer values, from 3 runs on `taskset -c 4`. These rows are the host to script calls, the script to native call, the script to script calls, the closure, the method rows, the module function and the call after N reloads. The other rows are from the older runs.

The `aot` and `aot-opt` values (the `aot-opt` table, the comparison tables and the lines that name `aot-opt`) are newer. They come from one Release build with `HOT_THREAD_SAFE=ON` (the default), on `taskset -c 6`, with the median of 3 runs of `bench_backends --backend aot` and `bench_backends_opt --backend aot-opt` on a quiet machine. The tables of `files`, `jit` and `aot` above come from a build with `HOT_THREAD_SAFE=OFF`, so the `aot` table and the `aot` column of the comparison tables differ in the `new` row and in some other cells.

Between the 3 runs, 84% of the cells changed by less than 3%. Some cells changed more. In each of them, one run was far from the two others, so the median is stable:

- the `jit` `.hot` script to script virtual call (20%), and `handle_call` and `module_var` in `aot` (10% and 60%)
- `new` and free (8% to 15%), and the string in `jit` for `.cpp` (13%)
- the native twin of the shell with a state block (12%)
- the reload in `jit` for `.hot` (11%), and the chain at N = 1 in `jit` for `.cpp` (16%)

## Results

Time in ns for one operation. The ratio is in parentheses. There is one table for each backend. The rows are the same in each table. The rows after `loop with a method call` test code that a reload can change. The native twin of these rows is the same code in plain C++. It uses a direct member access, a plain virtual call and a plain call.

### `files`

| Case | native | `.hot` | `.cpp` |
|---|---|---|---|
| empty method call (host to script) | 1.46 | 3.51 (2.4x) | 3.52 (2.4x) |
| native to script virtual call | 1.46 | 3.24 (2.2x) | 3.23 (2.2x) |
| script to script call (recursive) | 1.19 | 1.19 (1.0x) | 1.17 (0.98x) |
| script to native call | 1.46 | 1.46 (1.0x) | 1.46 (1.0x) |
| field read and write loop | 0.88 | 0.88 (1.0x) | 0.88 (1.0x) |
| integer arithmetic loop | 1.41 | 1.33 (0.95x) | 1.33 (0.94x) |
| float arithmetic loop | 1.75 | 1.75 (1.0x) | 1.75 (1.0x) |
| array index (`int[256]`) | 0.62 | 0.64 (1.0x) | 0.64 (1.0x) |
| `std.vector` index | 0.62 | 0.88 (1.4x) | 0.65 (1.0x) |
| handle dereference and null check | 9.85 | 10.5 (1.1x) | n/a |
| pointer dereference and null check | 0.41 | n/a | 0.80 (1.9x) |
| string interpolation or concatenation | 72.4 | 59.7 (0.82x) | 68.1 (0.94x) |
| `new` and free of a script object | 10.8 | 47.3 (4.4x) | 35.0 (3.2x) |
| closure call | 1.76 | 2.64 (1.5x) | 1.76 (1.0x) |
| loop with a method call | 1.26 | 1.25 (0.99x) | 1.26 (1.0x) |
| field of another object (handle or pointer) | 0.62 | 0.59 (0.95x) | 0.59 (0.95x) |
| small method call on `this` | 1.46 | 1.46 (1.0x) | 1.46 (1.0x) |
| method call through a handle or pointer | 1.46 | 1.46 (1.0x) | 1.46 (1.0x) |
| script to script virtual call | 1.75 | 2.64 (1.5x) | 2.20 (1.3x) |
| module variable read and write | 0.38 | 0.36 (0.94x) | 0.36 (0.95x) |
| module function and static method | 0.32 | n/a | 0.41 (1.3x) |

### `jit`

| Case | native | `.hot` | `.cpp` |
|---|---|---|---|
| empty method call (host to script) | 1.46 | 4.30 (2.9x) | 4.23 (2.9x) |
| native to script virtual call | 1.46 | 4.26 (2.9x) | 4.22 (2.9x) |
| script to script call (recursive) | 1.19 | 1.20 (1.0x) | 1.20 (1.0x) |
| script to native call | 1.46 | 1.46 (1.0x) | 1.46 (1.0x) |
| field read and write loop | 0.88 | 0.88 (1.0x) | 0.88 (1.0x) |
| integer arithmetic loop | 1.41 | 1.41 (1.0x) | 1.41 (1.0x) |
| float arithmetic loop | 1.75 | 1.75 (1.0x) | 1.75 (1.0x) |
| array index (`int[256]`) | 0.62 | 0.64 (1.0x) | 0.64 (1.0x) |
| `std.vector` index | 0.62 | 0.88 (1.4x) | 0.64 (1.0x) |
| handle dereference and null check | 9.85 | 10.4 (1.1x) | n/a |
| pointer dereference and null check | 0.41 | n/a | 0.80 (1.9x) |
| string interpolation or concatenation | 72.4 | 58.5 (0.81x) | 65.9 (0.91x) |
| `new` and free of a script object | 10.8 | 48.1 (4.5x) | 36.8 (3.4x) |
| closure call | 1.76 | 2.64 (1.5x) | 1.76 (1.0x) |
| loop with a method call | 1.26 | 1.25 (0.99x) | 1.26 (0.99x) |
| field of another object (handle or pointer) | 0.62 | 0.59 (0.95x) | 0.61 (0.99x) |
| small method call on `this` | 1.46 | 1.46 (1.0x) | 1.46 (1.0x) |
| method call through a handle or pointer | 1.46 | 1.46 (1.0x) | 1.46 (1.0x) |
| script to script virtual call | 1.75 | 2.10 (1.2x) | 2.20 (1.3x) |
| module variable read and write | 0.38 | 0.36 (0.94x) | 0.36 (0.94x) |
| module function and static method | 0.32 | n/a | 0.41 (1.3x) |

### `aot`

| Case | native | `.hot` | `.cpp` |
|---|---|---|---|
| empty method call (host to script) | 1.46 | 3.23 (2.2x) | 3.22 (2.2x) |
| native to script virtual call | 1.46 | 3.51 (2.4x) | 3.23 (2.2x) |
| script to script call (recursive) | 1.19 | 1.34 (1.1x) | 1.30 (1.1x) |
| script to native call | 1.46 | 1.46 (1.0x) | 1.46 (1.0x) |
| field read and write loop | 0.88 | 0.88 (1.0x) | 0.87 (1.0x) |
| integer arithmetic loop | 1.41 | 1.41 (1.0x) | 1.33 (0.94x) |
| float arithmetic loop | 1.75 | 1.75 (1.0x) | 1.75 (1.0x) |
| array index (`int[256]`) | 0.62 | 0.64 (1.0x) | 0.64 (1.0x) |
| `std.vector` index | 0.62 | 0.88 (1.4x) | 0.64 (1.0x) |
| handle dereference and null check | 9.85 | 10.4 (1.1x) | n/a |
| pointer dereference and null check | 0.41 | n/a | 0.80 (1.9x) |
| string interpolation or concatenation | 72.4 | 57.7 (0.80x) | 66.2 (0.91x) |
| `new` and free of a script object | 10.8 | 50.4 (4.6x) | 36.5 (3.4x) |
| closure call | 1.76 | 2.63 (1.5x) | 1.75 (0.99x) |
| loop with a method call | 1.26 | 1.26 (1.0x) | 1.26 (1.0x) |
| field of another object (handle or pointer) | 0.62 | 0.61 (0.99x) | 0.58 (0.94x) |
| small method call on `this` | 1.46 | 1.46 (1.0x) | 1.46 (1.0x) |
| method call through a handle or pointer | 1.46 | 1.46 (1.0x) | 1.46 (1.0x) |
| script to script virtual call | 1.75 | 2.19 (1.3x) | 2.17 (1.2x) |
| module variable read and write | 0.38 | 0.36 (0.94x) | 0.36 (0.94x) |
| module function and static method | 0.32 | n/a | 0.41 (1.3x) |

Some rows depend on the backend:

- The call from the host into a script (the first two rows) costs 3.2 to 3.5 ns in `files` and `aot`. It costs 4.2 to 4.3 ns in `jit`.
- The script to native call costs 1.46 ns in the three backends. The `files` and `jit` scripts call the host without a PLT jump (`-fno-plt`).
- The `.hot` script to script virtual call is 2.1 to 2.6 ns. The `.cpp` call is 2.2 ns.
- `new` and free of a `.hot` object cost 47.3 ns in `files`, 48.1 ns in `jit` and 50.4 ns in `aot`.

The string row is nearly the same in the three backends: `.hot` 0.80x to 0.82x and `.cpp` 0.91x to 0.94x.

The ratios of the recursive script to script call are from 0.98x to 1.1x. The native twin of this row changed by 7% between older runs. The code that the compiler makes is not the same. Do not read a difference from this row.

### `aot-opt`

The scripts are optimized AOT code. The values come from `bench_backends_opt --backend aot-opt --no-load` (see [Machine](#machine) for the build).

| Case | native | `.hot` | `.cpp` |
|---|---|---|---|
| empty method call (host to script) | 1.46 | 1.46 (1.0x) | 1.46 (1.0x) |
| native to script virtual call | 1.46 | 1.46 (1.0x) | 1.46 (1.0x) |
| script to script call (recursive) | 1.07 | 1.16 (1.1x) | 1.16 (1.1x) |
| script to native call | 1.46 | 1.46 (1.0x) | 1.46 (1.0x) |
| field read and write loop | 0.88 | 0.88 (1.0x) | 0.88 (1.0x) |
| integer arithmetic loop | 1.41 | 1.41 (1.0x) | 1.41 (1.0x) |
| float arithmetic loop | 1.75 | 1.75 (1.0x) | 1.75 (1.0x) |
| array index (`int[256]`) | 0.62 | 0.64 (1.0x) | 0.64 (1.0x) |
| `std.vector` index | 0.62 | 0.88 (1.4x) | 0.62 (1.0x) |
| handle dereference and null check | 9.85 | 10.4 (1.1x) | n/a |
| pointer dereference and null check | 0.41 | n/a | 0.44 (1.1x) |
| string interpolation or concatenation | 67.6 | 55.1 (0.8x) | 71.0 (1.1x) |
| `new` and free of a script object | 10.2 | 34.1 (3.3x) | 8.21 (0.8x) |
| closure call | 1.75 | 2.64 (1.5x) | 1.76 (1.0x) |
| loop with a method call | 1.25 | 1.26 (1.0x) | 1.25 (1.0x) |
| field of another object (handle or pointer) | 0.61 | 0.61 (1.0x) | 0.58 (0.9x) |
| small method call on `this` | 1.46 | 1.46 (1.0x) | 1.46 (1.0x) |
| method call through a handle or pointer | 1.46 | 1.46 (1.0x) | 1.46 (1.0x) |
| script to script virtual call | 1.46 | 1.75 (1.2x) | 1.46 (1.0x) |
| module variable read and write | 0.38 | 0.36 (0.9x) | 0.36 (0.9x) |
| module function and static method | 0.32 | n/a | 0.40 (1.3x) |

The reload and the reload chain have no row: optimized code has no reload. The tool prints `n/a` for them.

### `aot` against `aot-opt`

The same build, the same core and the same session. The ratio is script time divided by the native time of the same program.

| Case | native | `aot` `.hot` | `aot-opt` `.hot` | `aot` `.cpp` | `aot-opt` `.cpp` |
|---|---|---|---|---|---|
| empty method call (host to script) | 1.46 | 3.51 (2.4x) | 1.46 (1.0x) | 3.51 (2.4x) | 1.46 (1.0x) |
| native to script virtual call | 1.46 | 3.80 (2.6x) | 1.46 (1.0x) | 3.80 (2.6x) | 1.46 (1.0x) |
| script to script virtual call | 1.46 | 2.63 (1.8x) | 1.75 (1.2x) | 2.35 (1.6x) | 1.46 (1.0x) |
| `new` and free of a script object | 10.2 | 59.4 (5.8x) | 34.1 (3.3x) | 38.8 (3.8x) | 8.21 (0.8x) |

The other rows are equal in the two backends (within 0.1 ns, or within the noise of a run). The rows of the string, of the recursive call and of the pointer chain are in the noise of the build: the code is the same.

## How to read the ratios

- 1.0x is native speed. A ratio below 1.0x is noise or a small difference in the code that the compiler makes.
- The three backends give the same code speed. They differ in the time of a load, not in the time of a run. Only the calls across the host border differ.
- A loop of fields, numbers, arrays and calls inside one script runs at native speed. This is true for `.hot` and for `.cpp`.
- A call from the host into a script costs 1.8 to 2.1 ns more than a native virtual call in `files` and `aot`. In `jit` it costs about 2.8 ns more. This is true for `.hot` and `.cpp`. The call goes through a slot that a reload can change. It also catches the script errors.
- A `.cpp` script is as fast as a `.hot` script, or faster, in most cases. It is faster in four cases: `std::vector` (no check), the closure, the string and `new`.
- The `.hot` closure call costs 1.5x. The `.cpp` lambda in a `std::function` runs at native speed.
- The script to script call is 0.9x to 1.1x in both languages. The difference between the two is of the size of the run noise.
- Optimized AOT code (`aot-opt`) has no slot and no check for a changed script. A call from the host into a script costs the same as a native virtual call (1.0x), against 2.4x in `aot`. See [Optimized AOT](#optimized-aot-aot-opt).
- A ratio applies to the case only, not to a whole program. A ratio of 2.4x on a call of 1.46 ns is 2.0 ns more for each call.

### The pointer chain

The `.cpp` pointer chain costs 0.80 ns for each step. The native twin with raw pointers costs 0.41 ns, so the ratio is 1.9x. This row is not a null check cost. In `aot` a script object is a shell and a state block, so each step reads the state pointer of the shell, then the field. Two loads that depend on each other cost about twice one load. In `aot-opt` the fields are in the shell, and the step costs 0.44 ns (1.1x).

The `.hot` handle chain is 10.5 ns. The native twin of that row holds `std::shared_ptr` and counts every copy. It is 9.85 ns. The two chain rows have different native twins, so compare each with its own twin.

## The cost of a call

A micro benchmark copies the code of a host to script call and adds one step at a time. It ran on one core, with a second thread in the process, so the atomic operations are real. The values are the median of more runs. One CPU cycle is about 0.29 ns.

| Step of a host to script call | ns more | total |
|---|---|---|
| a native virtual call | | 1.46 |
| the shell override reads the slot of the class and calls the member function pointer | 0.88 | 2.34 |
| the script entry check (one load of the entry counters) | 0.88 | 3.22 |
| the check for a detached call (`slot_keys`) | 0.36 | 3.58 |
| the `try` and `catch` of `hot::ScriptError` (with a stack protector) | 0.81 | 4.39 |
| the call depth through an exported function (a PLT call) | 0.88 | 5.27 |

The old `hot::call` did all the steps, for 5.27 ns. The new `hot::call` keeps the same checks and the same error handling, but it does less work for them:

- One test covers the entry counters and the detached call. If one of them is on, the call goes to an out-of-line slow path.
- The call depth is a thread-local variable with the initial-exec model. The call does `incl %fs:...` and `decl %fs:...`, without a function call.
- The `catch` code is out of line. The fast path has no stack protector.

The result is 3.5 ns in `files` and 3.2 ns in `aot`. The JIT links its code without TLS relocations. So the `jit` code adds the thread pointer and the offset of the variable, which the backend gives as an absolute symbol. The compiler makes the same `incl %fs:...` instruction. The `jit` call is 4.2 to 4.3 ns. The `jit` code uses `-O1`, the other backends use `-O2`.

Other calls:

- A script to native call went through a PLT jump in `files` and `jit`. Now the module compiles with `-fno-plt`, and the call costs 1.46 ns, as in `aot`.
- A `.hot` closure was a `std::function` that held a `hot::Closure`, and the closure held a second `std::function`. Now the `std::function` holds the guarded lambda directly. The call has one indirect call, the entry check and the generation check: 2.6 ns, not 4.4 ns.
- A script to script virtual call has no `try`. It costs the same as the member function pointer call in the micro benchmark. A non-virtual script to script call is a direct call. These numbers are from before the frame guard. Now each script function adds one add, one compare and one subtract on the call depth, and an exception cleanup. This cost is not measured.

### Optimized AOT (`aot-opt`)

The slot and the checks around it are the cost of a host to script call (see the table above). Optimized AOT code (`MODE AOT OPTIMIZED`) removes them. The shell override calls the script member function directly, so the compiler sees the call. The fields and the script functions are members of the shell, so there is no state block and no `state_of` call. The call keeps the call depth and the `catch` of `hot::ScriptError`, so a script error still goes to the error handler. The 1.46 ns of `aot-opt` includes them.

| Call | `aot` | `aot-opt` | native |
|---|---|---|---|
| host to script (`empty`) | 3.51 | 1.46 | 1.46 |
| native to script virtual (`virtual_hook`) | 3.80 | 1.46 | 1.46 |
| script to script virtual (`.hot`) | 2.63 | 1.75 | 1.46 |
| script to script virtual (`.cpp`) | 2.35 | 1.46 | 1.46 |

The host to script call costs 2.05 ns less, and it is as fast as a native virtual call in this test. The script to script virtual call costs 0.9 ns less in `.hot` and 0.9 ns less in `.cpp`. In `.hot` it is still 0.29 ns more than the native twin. The calls that did not use a slot (the script to native call, the call on `this`, the call through a handle) have the same time in both backends.

## The cost of `new`

The `.hot` case makes an object with `new`, writes a field and frees the object. A script object has two parts. The shell is the object that native code sees. The state block holds the fields, and a reload can replace it. A `std::shared_ptr` owns the shell. One block holds the shell and the control block of the `std::shared_ptr` (as `std::make_shared` does). So one `new` makes two blocks of memory: the shell block and the state block.

The `.cpp` case does `new` and `delete` with a raw pointer. The `.cpp` object has a shell and a state block, but no `std::shared_ptr`.

The native twins do the same work with other owners. Each cell has two ratios, `.hot` / `.cpp`: the script time divided by the native time of the row.

| Native owner | native | `files` | `jit` | `aot` |
|---|---|---|---|---|
| `std::make_shared<T>()` | 10.8 | 4.4x / 3.2x | 4.5x / 3.4x | 4.6x / 3.4x |
| `std::shared_ptr<T>(new T)` | 17.7 | 2.7x / 2.0x | 2.7x / 2.1x | 2.9x / 2.1x |
| `std::make_unique<T>()` | 8.19 | 5.8x / 4.3x | 5.9x / 4.5x | 6.2x / 4.5x |
| raw `new T` and `delete` | 8.20 | 5.7x / 4.3x | 5.9x / 4.5x | 6.2x / 4.5x |
| intrusive pointer (atomic count) | 14.2 | 3.3x / 2.5x | 3.4x / 2.6x | 3.6x / 2.6x |
| virtual base + state block, `shared_ptr(new T)` | 38.8 | 1.2x / 0.90x | 1.2x / 0.97x | 1.3x / 0.96x |
| same, with a hash map of live objects | 64.5 | 0.72x / 0.55x | 0.75x / 0.57x | 0.78x / 0.57x |

The `.hot` time is 47 to 50 ns, and the `.cpp` time is 35 to 37 ns. The `.hot` object has a `std::shared_ptr` with an atomic count. The `.cpp` object has not. Three other runs gave up to 5 ns more for `.hot` in `jit` and `aot`.

The last two rows have the shape of a script object. The class has a virtual base. Its constructor allocates a state block and runs the field initializers through a table. The `.hot` script is 1.2x to 1.3x of the first of the two. That twin makes three blocks, and the script makes two. The `.cpp` script is at 0.90x to 0.97x of the same twin. The native time of the shell row changed by 12% between runs, so its ratios are less sure.

The last row has a hash map for the list of live objects. The script does not use a hash map: the list is in the object. So the script is faster than that twin.

A micro benchmark of the same object shows where the time of a `.hot` `new` goes. It ran on one core, in a process with more than one thread. The values are approximate (about 1 to 2 ns):

| Step | ns |
|---|---|
| `malloc` and `free` of the shell | 7.5 |
| `malloc` and `free` of the state block | 3.6 |
| the constructor of the shell and the virtual destructor call | 3.8 |
| the record, the owner pointer and the size of the block | 2.6 |
| the script entry and the destructor check of `destroy_state` | 2.5 |
| the field initializers and destructors through the table | 4.1 |
| the list of live objects (insert and erase) | 3 to 5 |
| the constructor lookup through the canonical record | 1 |
| the allocator of the shared block (`make_shared_shell`) | 3 |
| the control block and the atomic counts of the `std::shared_ptr` | 13 |

In the old layout, the `std::shared_ptr` had its own block, for 8 ns more. The old constructor lookup compared the key text, for 5 ns more. The atomic counts of `std::enable_shared_from_this` cost most of the last step. The base of a script class has it, because `this` can become a handle. A `std::weak_ptr` keeps the shell block until the `std::weak_ptr` goes. The destructor runs and the state block goes with the last `std::shared_ptr`.

### `new` in optimized AOT code

Optimized AOT code has one generation, so `new` makes the shell with a plain C++ `new` (or `std::make_shared`). It does not look up a constructor through the record and it does not note the generation of the object. The test ran `bench_backends` and `bench_backends_opt` in one build with `HOT_THREAD_SAFE=ON`.

| `new` and free of a script object | `aot` | `aot-opt` | Difference |
|---|---|---|---|
| `.hot` (`std::shared_ptr`) | 59.4 ns | 34.1 ns | 25.3 ns less |
| `.cpp` (raw pointer) | 38.8 ns | 8.2 ns | 30.6 ns less |

The ratios against each native owner (`.hot` / `.cpp`):

| Native owner | native | `aot` | `aot-opt` |
|---|---|---|---|
| `std::make_shared<T>()` | 10.04 | 5.9x / 3.9x | 3.4x / 0.82x |
| `std::shared_ptr<T>(new T)` | 17.86 | 3.3x / 2.2x | 1.9x / 0.46x |
| `std::make_unique<T>()` | 8.17 | 7.3x / 4.7x | 4.2x / 1.0x |
| raw `new T` and `delete` | 8.18 | 7.3x / 4.7x | 4.2x / 1.0x |
| intrusive pointer (atomic count) | 14.10 | 4.2x / 2.8x | 2.4x / 0.58x |
| virtual base + state block, `shared_ptr(new T)` | 41.11 | 1.4x / 0.94x | 0.83x / 0.20x |
| same, with a hash map of live objects | 65.13 | 0.91x / 0.60x | 0.52x / 0.13x |

The fields of an optimized object are members of the shell. The object is one allocation, and it has no state block, no table of field initializers and no list of live objects. `new` and free cost 34.1 ns in `.hot` and 8.2 ns in `.cpp`, against 59.4 ns and 38.8 ns in `aot`. The `.hot` time is the cost of the `std::shared_ptr` block and of the constructor chain. The `.cpp` object has no `std::shared_ptr`, so it costs the same as a native `new` and `delete` of a class with a virtual base. The rows of the native twins with a state block are for the comparison with `aot` only. The ratios of `aot-opt` use the same native times.

### The lock of `HOT_THREAD_SAFE`

The list of live objects has a spinlock when the build has `HOT_THREAD_SAFE=ON` (the default). Each `new` and each free takes the lock once. The numbers in the tables above come from a build without the lock (`HOT_THREAD_SAFE=OFF`). The test ran `bench_backends --backend files --filter alloc --no-load` in two Release builds, on one core (`taskset -c 4`), with the median of 3 runs. Each run is the median of 7 repeats. Both builds start one thread before the test, so `std::shared_ptr` uses atomic counts in both (libstdc++ uses plain counts in a process with one thread). Native `std::make_shared<T>()` took 10.0 ns in both builds.

| `new` and free of a script object | `HOT_THREAD_SAFE=OFF` | `HOT_THREAD_SAFE=ON` | Difference |
|---|---|---|---|
| `.hot` (`std::shared_ptr`) | 44.2 ns | 61.6 ns | +17.3 ns |
| `.cpp` (raw pointer) | 29.6 ns | 34.4 ns | +4.8 ns |

The lock is one atomic exchange to take it and one plain store to release it. The cost is the exchange itself. An experiment with a plain store in place of the exchange (not correct, for the test only) gave the `OFF` times again. Two changes did not help: the lock flag next to the list data, and a test-and-test-and-set lock (the code keeps the second, because it writes less under contention). An atomic exchange is a full barrier on x86, and it waits until the pending stores of the allocation are written. The `.hot` case does more work around it than the `.cpp` case, and it also has the atomic counts of the `std::shared_ptr`, so it pays more. I did not measure the reason inside the CPU, because `perf` is not on this machine. If one thread creates and destroys all the script objects of your program, set `HOT_THREAD_SAFE=OFF` to remove this cost. See [Thread safety](cmake-options.html#thread-safety).

## Reloadable access and calls

The rows after `loop with a method call` in the tables above test code that a reload can change.

- A field access through a handle (`.hot`) or a pointer (`.cpp`) stays at native speed: 0.94x to 1.0x. The compiler inlines the method and moves the generation check out of the loop.
- A method call through a handle or a pointer costs 1.46 ns, as the native call. A small method call on `this` costs the same.
- A virtual call from script to script reads a slot. It costs 0.35 to 0.9 ns more than a native virtual call, depending on the backend and the language.
- A module variable read and write is 0.94x.
- A `.hot` free function and static method are not measured yet. The module function row has a `.cpp` script only. The `.cpp` call costs 0.41 ns for the pair, against 0.32 ns for the twin: 1.3x. That is 0.1 ns more.

### Reload cost

The tool reloads 100 live objects with 65 fields. The new version adds a field, so each object migrates. The time covers `request_reload` and `commit_pending`. It does not cover the compile.

| Backend | `.hot` | `.cpp` |
|---|---|---|
| `files` | 0.101 ms | 0.089 ms |
| `jit` | 1.42 ms | 1.33 ms |
| `aot` | n/a | n/a |

The `.cpp` and `.hot` times are about the same. The two reloads have the same fields and the same migration.

### A call after N reloads

The host calls a script override (`Hook`) that reads a field. The object is made in generation 1 and the tool reloads the script N times. Each reload adds or removes a field, so the state of the object migrates. The native virtual call takes 1.46 ns. The table shows the old object. The `aot` backend has no reload.

| N | `files` `.hot` | `files` `.cpp` |
|---|---|---|
| 0 | 3.40 (2.3x) | 3.42 (2.3x) |
| 1 | 3.41 (2.3x) | 3.41 (2.3x) |
| 10 | 3.42 (2.3x) | 3.41 (2.3x) |
| 100 | 3.41 (2.3x) | 3.41 (2.3x) |

| N | `jit` `.hot` | `jit` `.cpp` |
|---|---|---|
| 0 | 4.30 (2.9x) | 4.31 (3.0x) |
| 1 | 4.25 (2.9x) | 4.33 (3.0x) |
| 10 | 4.31 (2.9x) | 4.31 (3.0x) |
| 100 | 4.16 (2.9x) | 4.32 (3.0x) |

The cost does not grow with the number of reloads. A new object, made after the last reload, gives the same time: within 1% in each cell.

## Caveats

- The cases are small loops. A real program has other costs: cache misses, the size of the code and the calls between modules.
- A twin is only as fair as its code. The string twin uses `std::to_string`. The `.hot` script uses `${}` (`fmt`). The `.cpp` script uses `std::to_string`, as the twin does. So the `.hot` string row is a rough value. It is also the one row where `.hot` is more than 10% faster than native (0.80x to 0.82x). The cause is `fmt` against `std::to_string`.
- A `.cpp` script is a normal C++ file, and the compiler sees all of it. The native twin hides some values from the compiler (for the string, the closure and `new`). So a ratio of a `.cpp` script below 1.0x can come from this difference.
- The `.cpp` twins with pointers have no generation check. The `.hot` handles have it. So these rows do not show the cost of a handle in `.hot` against a pointer in `.cpp` only. The language and the owner differ.
- The native time of the script to script virtual call was 1.75 ns in this run. Another build of the tool gave 1.46 ns. The native time of a virtual call depends on the layout of the code. Compare the script with the native twin of the same run.
- libstdc++ uses plain (not atomic) `std::shared_ptr` counts while a process has only one thread. The tool starts a thread first, so all cases use the atomic counts, as in a host with threads.
- The compiler can move work out of a loop in one case and not in its twin. The cases use a value that each iteration needs, but a ratio near 1.0x can still hide a small cost.
- `HOT_RUNTIME_CHECKS=OFF` removes the null and index checks of `.hot` code. Build again with that option to compare. The tool prints the value of the option. A `.cpp` script has no such checks.
- The `aot` backend has no reload. The reload cases run in `files` and in `jit`.
- The `aot-opt` backend has no reload and no override. The loader refuses both. A program that needs a reload uses `aot` (`MODE AOT_RELOAD`).
- `bench_backends_opt` is a separate program, so its native twin is a separate copy of the code. The native times of the two programs differ by up to 0.1 ns. The tables of the comparison use the ratio of each program.
- One machine and one compiler. Measure again on your own machine and with your own headers.

## Script start-up cost (BiteSizedWar, files backend)

The environment variable `HOT_TIMING` makes the files backend print one line to `stderr` for each unit build: `hot: <unit> translate <ms> ms, compile <ms> ms, link <ms> ms`. The numbers below come from the earlier single clang run (compile and link in one command).

The numbers come from this machine (32 hardware threads, clang 23.1.3, Debug build of the game, cold start with no cache).

Cold start of `BiteSizedWarHot --offline --headless`:

| Value | Result |
|---|---|
| Units built | 75 |
| Wall time, process start to `loaded N scripts` | 424.7 s |
| Sum of the translate times | 154.2 s |
| Sum of the clang times | 269.3 s |

- The units build one after the other. The sum of the two times (423.5 s) is 99.7% of the wall time.
- The compile has no `-O` flag, so clang uses `-O0`. The command has `-g`, `-fPIC`, `-fwrapv` and `-fno-plt`.

The five biggest units by clang time:

| Unit | Translate | Clang |
|---|---|---|
| `Game/Game` | 28149 ms | 14660 ms |
| `Carve/CarveCheck` | 5712 ms | 9180 ms |
| `Carve/Destruction` | 2822 ms | 6635 ms |
| `Effects/ProjectileEffects` | 4791 ms | 6437 ms |
| `Weapons/ShotResolver` | 3643 ms | 6039 ms |

The compile of the biggest unit (`Game/Game`: 50244 lines, 3.1 MB of C++) by hand, with the same flags:

| Step | Time |
|---|---|
| `-fsyntax-only` (parse and checks, no code) | 10.5 s |
| `-c -o x.o` (compile) | 14.9 s |
| `clang++ -shared x.o -o x.so` (link) | 0.29 s |
| `-shared` in one command | 15.2 s |
| `-fsyntax-only` of only the 137 `#include` lines of the unit (87 Urho3D headers) | 6.0 s |

- The link is 2% of the build. The compile is the cost.
- The headers alone take 6.0 s of the 14.9 s. The rest is the code of the unit.
- A cache of the object file or of the `.so` can skip all of this for a unit that did not change.

### With the object cache

The game sets the cache folder (`ScriptSystem::SetCacheDir`). The translator is now built with `-O2` in Debug. This is why the cold translate sum is lower than in the table above. The numbers come from `HOT_TIMING=1 ./BiteSizedWarHot --offline --headless`, same machine.

| Run | Wall time | Units | Hits | Misses | Translate sum | Compile sum | Link sum |
|---|---|---|---|---|---|---|---|
| Cold (empty cache) | 372 s | 75 | 0 | 75 | 101.0 s | 258.3 s | 8.9 s |
| Warm (same command) | 114 s | 75 | 75 | 0 | 99.8 s | 0.0 s | 8.5 s |
| Edit of `World/SpawnPoint.hot` | 126 s | 75 | 73 | 2 | 96.5 s | 16.3 s | 8.2 s |

- The wall time is the time from the first log line to `loaded N scripts`. The log time stamps have a 1 s step.
- The warm run misses nothing. A restart starts at generation 1, so the generated text is the same.
- The warm run still translates all units. The translate cache is a later step. The translate sum is now 88% of the warm wall time.
- The edit adds one method to a leaf script. Two units miss: the edited unit and `Game/Game`, which imports it.
- The cache folder is 128 MB for 75 units.

## Translate cost (BiteSizedWar)

This section says where the translate time of the start-up goes. The translate sum is 154.2 s for 75 units. `Game/Game` takes 28.1 s of it.

### Is the session shared?

Yes. One `hotc::TranslatorSession` serves all units of a run. `ClangFilesBackend` creates it in its constructor (`src/runtime/clang_files_backend.cpp:236`). `translate()` passes it at `src/runtime/clang_files_backend.cpp:528`. `ScriptSystem::EnsureBackend` makes one backend (`rbfx-script/Plugins/Rbfx.Script.Core/ScriptSystem.cpp:535`).

But the session does not help between units. The cache key is the flags, the directory and the native `using` lines of the unit (`src/translator/hotc_lib.cpp`, `session_bases`). A unit with other native imports has another key. It parses the native headers again. One `hotc` process with one session shows this (RelWithDebInfo `hotc`, same flags as the game, warm disk cache):

| Call in one session | Time | `parse_count` |
|---|---|---|
| `Game/Game` (first) | 7674 ms | 1 |
| `Game/WorldLayers` | 510 ms | 2 |
| `Player/PlayerSettings` | 1541 ms | 3 |
| `Game/Combatant` | 1495 ms | 4 |
| `Game/WorldLayers` again | 1 ms | 4 |
| a script with only the 43 native `using` lines of `Game/Game` | 2884 ms | 5 |

Each new import set adds one libclang parse. A call with a known key takes 1 ms. The first unit is not much slower than the others. In the game log the cost is per unit: the 16 units below 50 lines take 0.9 s on average (median 0.93 s), the units of 50 to 150 lines take 1.6 s.

### The game is built without optimization

`libhotc_lib` is built with `g++ -g` and no `-O` in the game build (`compile_commands.json` of `BiteSizedWar/cmake-build/linux-gcc-x64-dll`). Only libclang is optimized. The same translate of `Game/Game` with the `hotc` of this repository:

| Build of the translator | First call | Second call (warm session) |
|---|---|---|
| `-O2 -g` (clang) | 7.1 to 7.6 s | 3.1 to 3.2 s |
| `-g` (clang, no `-O`) | 21.9 s | 17.7 s |
| `-g` (g++, no `-O`), in the game | 28.1 s | not measured |

So 21 s of the 28 s are the missing optimization. The time of libclang does not change (4.2 s).

### Phases of `Game/Game`

Temporary timers around the calls in `translate_impl` gave this split. The `-g` column is the no-`-O` build, which is close to the game.

| Phase | `-O2` | `-g`, no `-O` | Share of 22.3 s |
|---|---|---|---|
| `analyze_param_use` (parameter analysis, `check.cpp`) | 2.5 s | 10.3 s | 46% |
| of that, `ctx.for_class` in `ParamAnalysis::receiver_call` (4808 calls) | 2.0 s | 9.1 s | 41% |
| `parse_structure` (96 calls: the prelude, the 53 imported scripts and the script) | 0.29 s | 5.7 s | 26% |
| libclang parse of the native headers (`session_bases`) | 4.2 s | 4.4 s | 20% |
| `lower_block` | 0.02 s | 0.14 s | 0.6% |
| `check_nulls` and `lint_class` | 0.01 s | 0.04 s | 0.2% |
| `check_class` | 0.005 s | 0.03 s | 0.1% |
| `infer_const` | 0.006 s | 0.03 s | 0.1% |
| `collect_native_queries` | 0.003 s | 0.02 s | 0.1% |
| the rest (emit, the merge of imports, the types) | 0.8 s | 1.9 s | 8% |

The no-`-O` run is 22.3 s, and the game takes 28.1 s with g++. Apply the shares to the 28 s.

- The parameter analysis is the top cost. `ParamAnalysis::fields_written` runs for each parameter (639 top calls). For each call `o.F()` on another object it calls `ctx.for_class(cd)` for each script class that has a function `F`. This builds a `ParamContext` again every time (0.4 ms at `-O2`). The result does not depend on the parameter. Nothing caches it.
- `nullcheck.cpp`, `const_infer.cpp`, `lower.cpp` and the other checks are below 1% together.

### Growth with size

The translate time of the 74 other units against the lines of the script (`HOT_TIMING` log, the `-g` g++ build):

| Lines of the script | Units | Mean translate | Median |
|---|---|---|---|
| below 50 | 16 | 915 ms | 934 ms |
| 50 to 149 | 20 | 1584 ms | 1612 ms |
| 150 to 399 | 22 | 1487 ms | 1702 ms |
| 400 to 899 | 11 | 2569 ms | 2720 ms |
| 900 and more | 5 | 3750 ms | 3112 ms |

A fit over these 74 units gives about 780 ms + 0.49 ms for each line + 80 ms for each native `using`. The floor is the libclang parse. The correlation with the number of `using` lines (0.83) is better than with the lines (0.72). `Game/Game` is far above the fit (about 2.7 s expected, 28.1 s measured). It has 53 imported scripts. The parse of the imports and the parameter analysis grow faster than the line count. The growth is probably more than linear in the number of script classes with the same function name.

### The libclang part

A script with only the 43 native `using` lines of `Game/Game` (no `.hot` imports) takes 2.9 to 3.7 s for the libclang parse. The full `Game/Game` takes 4.2 s, because its 53 imported scripts add native headers (137 `#include` in the probe, 87 Urho3D). The figure of about 400 ms in `doc/internals.md` is for a small header set. For the engine headers one parse costs 1.5 to 4 s. It is 20% of `Game/Game`. For the small units it is the main cost, and it runs again for each unit.

### Recommendation

1. Cache the `for_class` result in `ParamAnalysis` (`src/translator/check.cpp`, `receiver_call`). One `ParamContext` for each class per translate, or the set of fields that each function writes, each memoized. Expected gain: about 40% of the translate of `Game/Game` (about 11 s of 28 s), and most of the translate time of the big units.
2. Build `hotc_lib` with `-O2` in the game build (the files backend, for the `Release` and `Debug` configs of the host). Expected gain: 3 to 4 times on all translate time (about 120 s of the 154 s, before change 1).
3. Next: share the libclang parse between units (one parse of the union of the native headers, or a key of the flags only). Expected gain: 1 to 3 s for each unit, about 80 s of the 154 s.
4. Last: cache the parse of an imported script by the hash of its text.

## The precompiled header of the files backend

The numbers come from BiteSizedWarHot (`HOT_TIMING=1 ./BiteSizedWarHot --offline --headless --quit-after 60`), clang 23, `-g`, same machine. The small unit `Game/GameDevCommands` compiles in 5.0 s. With `-ftime-trace`, the frontend takes 4.8 s of 5.4 s, and the backend (codegen and optimization) 0.6 s. A file with only the 59 include lines of the unit takes 3.5 s (`-fsyntax-only`). So the parse of the native headers is about 70% of the compile. The 87 units have 78 different include sets and one flag set.

| Unit | Compile without PCH | PCH build (in a thread, once) | Compile with PCH |
| --- | --- | --- | --- |
| `Game/GameDevCommands` (manual run) | 5.0 s | 4.5 s | 1.7 s |
| `Game/GameServerSide` (464 lines) | 7.6 s | 5.5 s | 3.2 s |
| `Game/Game` (2144 lines) | 13.4 s | 6.0 s | 9.1 s |

A cold build is the same: the first miss of an include set builds no PCH. The warm batch stays at 8.0 s (87 hits). The batch of one edit of `Game/GameServerSide` goes from 24.0 s to 19.5 s, and of `Game/Game` from 26.1 s to 21.8 s. A PCH is 50 to 70 MB.

## Parallel compiles

The launcher compiles a batch in parallel (`HOT_JOBS`). The measurement of the gain is pending. The numbers above come from the serial build.

## Translate time by phase (cold start of the BiteSizedWar game)

The `hot:` line of a translate miss shows the phases after the old fields. HOT_TIMING=1 enables it.
One cold start: 87 units, an empty cache, HOT_JOBS not set. Each unit has one parse.

| Item | Value |
|---|---|
| Sum of translate | 130825 ms |
| Sum of parse (libclang) | 116606 ms (P = 89.1 %) |
| Sum of lock wait | 0 ms |
| Sum of front (before the session lookup) | 2854 ms (F = 2.2 %) |
| Sum of lower (session lookup to first check) | 0 ms |
| Sum of check (first check to emit start) | 3919 ms |
| Sum of emit | 136 ms |
| Parses for each unit | 1.0 |
| Parse of the 5 smallest units (ms) | 216, 217, 218, 218, 223 |
| 5 largest translate times (ms) | Game/Game 4477, Game/GameServerSide 3504, Game/GameClientMessages 3196, Game/GameMatch 3047, Game/GameCarveFlow 2764 |

The phases cover 123515 ms of 130825 ms. The rest is the session lookup work outside the parse.
The check phase includes the per-class lowering. The emit phase starts at the function bodies.
The smallest parse is about 220 ms: this is the fixed cost of one parse.

## Parallel translate (cold start of the BiteSizedWar game)

The launcher and the script system start the loads of one wave with `Backend::begin_load_many`.
The units of a wave translate on `build_jobs()` threads. The setup is the same as above: 87 units,
an empty cache, HOT_JOBS not set, 32 hardware threads, a Debug build of the game.

| Item | Serial translate | Parallel translate |
|---|---|---|
| Wall time of the cold start | 206 s | 100 s (second run 107 s) |
| Sum of translate | 131 s | 256 s |
| Translate phase (first to last translate output) | about 131 s | about 13 s |
| Sum of lock wait (`wait`) | 0 s | 6.4 s (2 units) |
| Peak memory (VmHWM of the game) | not measured | 4.7 GB |
| Errors in the game log | 0 | 0 |

The sum of translate grows because the units share the processor. The translate phase time comes
from the file times of the generated sources. The two units with a lock wait share one session
entry with another unit.

## Compile database write (cold start of the BiteSizedWar game)

The files backend wrote `compile_commands.json` for each unit. The file has 957 entries and 13 MB.
The cost grew with each unit. In a cold start of 87 units, the sum of `record_stage` was 74 s.
The sums of `complete_load` and `dlopen` were 98 ms and 58 ms.
HOT_TIMING prints the sums in the `hot: UNIT finish` line.

Now a `begin_load_many` call writes the file once, when the last unit of the batch ends.
The destructor of the backend writes the file if an entry is not written yet.

| Start | Wall | Batch line |
|-------|------|------------|
| Cold, before | 117.5 s | 114609 ms |
| Cold, after | 43.0 s | 40086 ms |
| Warm, after | 4.0 s | 1881 ms |

The sum of `record_stage` after the change is 68 ms.

## Edit-to-reload latency

This section measures the time from an edited script to the new code in the process. The files backend does the work (translate, compile, link, load). The tool is `bench_reload` with the script `tools/bench/bench_reload.py`. The numbers are from BiteSizedWar, clang 23, the `-g` build of the flags of `WeaponHolder.cpp`, and the same machine as above.

**Method.** One run is one new process with an empty cache folder. It does one load that is not timed, one warm-up reload and 12 timed reloads. Each reload edits one number in the body of a method. The table shows the median of the 12 reloads. The `.hot` script is `WeaponHolder.hot` (a real BiteSizedWar script, the generated unit has about 4700 lines). The `.cpp` script is `tools/bench/reload/heavy_reload.cpp`. Two rounds ran, and each round ran each variant once (alternated). The load average was 1.6 to 2.2 (the tool prints it before each run; 2.0 is the limit for a quiet run, and the second round was at 2.0 to 2.2). Run it with:

```
python3 -I tools/bench/bench_reload.py --exe build/bin/bench_reload --out /tmp/bench --kind hot --reps 12 --load-limit 99
python3 -I tools/bench/bench_reload.py --exe build/bin/bench_reload --out /tmp/bench --kind cpp --reps 12 --load-limit 99
```

The switches in the table below make the baselines: `HOT_SPLIT_UNIT_OBJECTS=0`, `HOT_SHAPE_PCH=0`, `HOT_PCH_INSTANTIATE=0`. The line "all steps off" also sets `HOT_DIRECT_LINK=0` and `HOT_FMT_HEADER_ONLY=1`.

| Script | Variant | Total (ms) | Translate | Compile | Link |
|---|---|---|---|---|---|
| `.hot` | all steps on | 490 / 489 | 50 | 408 / 406 | 30 |
| `.hot` | `HOT_SPLIT_UNIT_OBJECTS=0` | 603 / 600 | 50 | 522 / 518 | 29 |
| `.hot` | `HOT_SHAPE_PCH=0` | 756 / 757 | 50 | 674 / 677 | 29 |
| `.hot` | all steps off | 1052 / 1046 | 50 | 948 / 945 | 49 |
| `.cpp` | all steps on | 531 / 533 | 122 | 379 / 382 | 27 |
| `.cpp` | `HOT_PCH_INSTANTIATE=0` | 1085 / 1085 | 675 | 381 / 382 | 27 |
| `.cpp` | all steps off | 1377 / 1376 | 675 | 651 / 652 | 48 |

(Two values: round 1 / round 2.) The first reload after a process start (the warm-up reload, a cold cache folder) takes 758 ms for `.hot` and 530 ms for `.cpp`: the shape PCH is not ready, so the compile reads the whole unit. The first load takes 5.5 s (`.hot`) and 7.3 s (`.cpp`). Before this work the `.hot` reload took about 1380 ms, and the `.cpp` reload took about 6970 ms (the first measurement, load 4 to 5).

**What each step gave** (medians of the runs of that step, so the loads differ a little):

| Step | Effect |
|---|---|
| Link with lld | `.hot` link 200 -> 52 ms. |
| Parallel probe test of the shared PCH | the probe path test of `begin_load_many` runs on all cores. |
| PCH of the native includes of a `.cpp` unit | `.cpp` compile 3549 -> 668 ms, total 5965 -> 3020 ms. |
| Kept libclang unit of a `.cpp` script (preamble reuse) | `.cpp` translate about 2300 -> 680 ms, total 3020 -> 1380 ms. |
| `fmt` of the host in the modules | `.hot` total 1050/1107 -> 782/785 ms, `.cpp` total 1379/1406 -> 1110/1130 ms. |
| Shape PCH of a `.hot` unit | `.hot` total 782 -> about 620 ms. |
| Two objects for a `.hot` unit | `.hot` total 621 -> 508 ms (-113 ms). The join (`ld.lld -r`) costs about 27 ms and the guard about 8 ms. |
| Template instances in the libclang preamble | `.cpp` translate 677 -> 122 ms, total 1105 -> 550 ms (-555 ms). |
| Direct linker command | link 49 -> 30 ms (-18 ms) for `.hot` and 48 -> 27 ms for `.cpp`. |

The probe test of the shared PCH (`prewarm_pch`) takes 16 to 40 ms (the list of the BiteSizedWar cache has about 13000 paths). A cache of the directory time stamps saved about 4 ms, so the backend does not use it. The rest of the `.hot` reload (490 ms) is the compile of the bodies (about 380 ms of clang) and the read of the generated unit. The rest of the `.cpp` reload is the compile (about 380 ms), the libclang reparse (68 ms) and the translate after it.

## Reload after an interface change (BiteSizedWarHot, HOT-54)

This section measures the reload of one edit of `Game/EffectClock.hot` in the game BiteSizedWarHot. The host decides with `importer_is_stale` which importers it recompiles (HOT-12 steps A to C). The ticket asks if a shared PCH before the first edit (steps 8 and 9 of `reload-plan.md`) is needed.

**Method.** The program is the Debug build of BiteSizedWarHot (`HOT_TIMING=1 ./BiteSizedWarHot --offline --headless`), clang 23, loose scripts, 23 enabled scripts, the object cache and the shared PCH of an earlier run on the disk (a warm start). The scripts are from a worktree of BiteSizedWar (master `6a23bc5`). A driver script changes `EffectClock.hot`, waits for the line `reload done in X (N scripts, K dependents kept without compile)` and restores the file with `git checkout`. Each edit has a number that is new in each run, so no run is an object cache hit. One process does the 5 edit kinds in this order: first edit after the start (a body edit), body, unused new method, new field, new overload `GetEffectStep(float)`. Three processes ran. "Wall" is the time from the write of the file to the log line (poll of the driver 0.2 s, the poll of the file change of the host adds up to 0.5 s).

The machine is the 32 thread machine of the other sections. It was not quiet: the load average was 13 to 27 because of other work (before the run: 13.5). The numbers are an upper bound of a quiet machine.

| Edit kind | Reload time of the 3 runs (ms) | Median (ms) | Wall median (s) | Units compiled | Dependents kept without compile |
|---|---|---|---|---|---|
| Start (not an edit) | 2200 / 2000 / 2000 | 2000 | 3.3 | 23 (object cache hits) | 0 |
| First edit after the start (body) | 2300 / 2100 / 2100 | 2100 | 2.8 | 1 | 0 |
| Body (a constant in a method) | 919 / 1100 / 919 | 919 | 1.4 | 1 | 0 |
| Unused new method | 822 / 844 / 822 | 822 | 1.2 | 1 | 17 |
| New field | 954 / 879 / 839 | 879 | 1.4 | 1 | 17 |
| New overload `GetEffectStep(float)` (the name is in importers) | 6400 / 6600 / 6800 | 6600 | 6.6 | 7 | 11 |

Notes:
- `EffectClock` has 17 importers. An unused method or a new field changes no name that an importer uses, so the host keeps all 17 importers without a compile. The reload costs the same as a body edit.
- The new overload makes the 6 importers that use the name `GetEffectStep` stale. The unit and 6 importers compile (7 scripts). The 11 other importers stay. The old rule (hash only) made 18 units compile in 27 to 47 s (WAR-2).
- The first edit after the start takes 2.1 s. A later body edit takes 0.9 s. The shared PCH is not the cause: the line of the first edit has `pch` and the plan finds the PCH of the earlier run. The difference is the translate phase: 1415 ms with a libclang probe parse of 1324 ms, against 106 to 196 ms for the later edits (the parse is not repeated). The first edit also builds the shape PCH of the unit (134 ms, in parallel).
- The compile of the unit takes 0.5 to 0.7 s and the link about 0.1 s in each edit. The `flush_compile_commands` phase takes 2 to 4 ms (it took 4 s in WAR-2).
- A cold cache folder (no PCH and no objects on the disk) was not measured here. WAR-2 measured the first reload of such a start at 8.7 s, with a shared PCH build of 6 s.

## HOT-59 prewarm of the compile database

`Backend::prewarm(sources)` starts one low-priority thread. The thread reads the compile database before the first edit. The first `flush_compile_commands` then compares and writes only. It does not read the file (about 350 ms of the first flush in the real game). `HOT_PREWARM=0` turns it off. The PCH prewarm stays in `begin_load_many`: it needs the translated jobs of the load.
