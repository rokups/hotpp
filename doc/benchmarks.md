---
layout: default
title: Benchmarks
nav_order: 43
---

The tool `bench_backends` measures the run-time cost of script code. Each case has up to three versions that do the same work:

- a `.hot` script,
- a [`.cpp` script](cpp-scripts.html): a plain C++ file that `hotc` translates (the libclang frontend),
- a native C++ twin.

The native twin is the reference. The result is a ratio: script time divided by native time.

## How to run

Build the tool in a Release build. A build with no `CMAKE_BUILD_TYPE` compiles the runtime without optimization, and the ratios are larger. The tool is part of the build when `HOT_BUILD_TESTS` is `ON`.

    cmake -S . -B build-rel -G Ninja -DCMAKE_BUILD_TYPE=Release -DHOT_BUILD_TESTS=ON
    cmake --build build-rel --target bench_backends
    build-rel/bin/bench_backends --no-load

One binary tests every backend of the build:

- `files`: the clang files backend compiles each script at run time (`-O2`).
- `jit`: the JIT backend compiles each script in the process (`-O2`).
- `aot`: the scripts are C++ in the binary (`MODE AOT_RELOAD` of `hot_add_host`, `-O2`).

The native twins also use `-O2`. A backend that the build does not have is skipped. The `.hot` scripts and the `.cpp` scripts are built in the same way in each backend.

The table of the tool has two columns for each backend, `hot` and `cpp`. Each column has a time and a ratio. The text `n/a` marks a case that the language cannot write.

| Option | Effect |
|---|---|
| `--backend all\|files\|jit\|aot` | test one backend only |
| `--filter text` | run only the cases whose name contains the text |
| `--reps N` | timed runs of each case; the result is the median (default 7) |
| `--iters N` | loop count of each case |
| `--reloads N` | the largest N of the reload chain (default 100) |
| `--csv file` | write the table also as CSV |
| `--no-load` | skip the load section (translate and compile time) |
| `--list` | print the backends and the cases, with the `.hot` and the `.cpp` file of each case |

The tool checks that each script gives the same result as its twin. A `!` after a time marks a different result.

Run the tool alone, on a quiet machine. Other work on the machine changes the numbers. One run of all backends takes about 4.5 minutes. Each case and each reload needs one compile in `files` and in `jit`. Pin the process to one core to get less noise: `taskset -c 6 build-rel/bin/bench_backends --no-load`.

## The three versions of a case

The scripts are in `tools/bench/scripts`. A `.hot` script has the name of the case, for example `fields.hot`. Its `.cpp` twin has the prefix `cpp_`, for example `cpp_fields.cpp`. Both derive from the same native base class and run the same loop. The checked result is the same.

A `.cpp` script is plain C++. Its arrays and its `std::vector` have no bounds check, as in any C++ code. A `.hot` array and a `.hot` `std.vector` have a check when `HOT_RUNTIME_CHECKS` is `ON`.

Some cases exist in one script language only:

| Case | `.hot` | `.cpp` | Reason |
|---|---|---|---|
| handle dereference and null check | yes | n/a | A `.cpp` script has no script class handles. It uses raw pointers, which have no null or generation check. |
| `new` and free of a script object | yes | n/a | The object of a `.hot` script is a handle with a shell and a state block. A raw `new` in a `.cpp` script does other work. |
| field of another object | yes | n/a | This case needs a handle. |
| method call through a handle | yes | n/a | This case needs a handle. |
| script to script virtual call | yes | n/a | The case calls through a handle to a script class that has a script virtual method. |
| module function and static method | n/a | yes | A `.hot` script has no free functions and no static methods. |

## Machine

The CPU is an AMD Ryzen 9 3950X (a desktop CPU, 16 cores, 32 threads). The software is Linux 7.2, glibc 2.44, clang 23.1.3 and libstdc++. The build uses `CMAKE_BUILD_TYPE=Release`, `HOT_RUNTIME_CHECKS=ON` and `HOT_TRACEBACK=OFF`. The process runs on one core (`taskset -c 6`). The values are the median of 3 runs of the tool (each run gives the median of 7 timed runs). Between the 3 runs, most rows changed by less than 3%. These rows changed more:

- the script to script call (the native twin changed by 11%, the scripts by 2% to 5%)
- the script to native call in `jit` for `.hot` (14%) and in `aot` for `.cpp` (18%)
- string concatenation (up to 6%)
- `new` and free (up to 5%)
- the reload in `jit` (10%)

## Results

Time in ns for one operation. The ratio is in parentheses. The values are from the `aot` backend. The three backends give the same speed for code that runs inside a script. Only the calls across the host border differ (see [By backend](#by-backend)).

| Case | native | `.hot` | `.cpp` |
|---|---|---|---|
| empty method call (host to script) | 1.46 | 4.97 (3.4x) | 4.98 (3.4x) |
| native to script virtual call | 1.46 | 5.00 (3.4x) | 4.97 (3.4x) |
| script to script call (recursive) | 1.14 | 1.20 (1.1x) | 1.33 (1.2x) |
| script to native call | 1.46 | 1.46 (1.0x) | 1.46 (1.0x) |
| field read and write loop | 0.88 | 0.88 (1.0x) | 0.88 (1.0x) |
| integer arithmetic loop | 1.41 | 1.41 (1.0x) | 1.41 (1.0x) |
| float arithmetic loop | 1.75 | 1.75 (1.0x) | 1.75 (1.0x) |
| array index (`int[256]`) | 0.62 | 0.64 (1.0x) | 0.64 (1.0x) |
| `std.vector` index | 0.62 | 0.88 (1.4x) | 0.64 (1.0x) |
| handle dereference and null check | 9.85 | 10.5 (1.1x) | n/a |
| string interpolation or concatenation | 68.9 | 55.3 (0.80x) | 66.2 (0.96x) |
| `new` and free of a script object | 10.4 | 58.6 (5.6x) | n/a |
| closure call | 1.75 | 4.39 (2.5x) | 1.75 (1.0x) |
| loop with a method call | 1.25 | 1.25 (1.0x) | 1.26 (1.0x) |

### By backend

Some rows depend on the backend. The `.cpp` script has the same time as the `.hot` script in each of these rows, within 2%. A row with `n/a` has no `.cpp` script.

| Case | native | `files` | `jit` | `aot` |
|---|---|---|---|---|
| empty method call (host to script) | 1.46 | 5.55 (3.8x) | 5.85 (4.0x) | 4.97 (3.4x) |
| native to script virtual call | 1.46 | 5.56 (3.8x) | 5.84 (4.0x) | 5.00 (3.4x) |
| script to native call | 1.46 | 2.04 (1.4x) | 2.04 (1.4x) | 1.46 (1.0x) |
| script to script virtual call (`.hot` only) | 1.46 | 2.18 (1.5x) | 2.63 (1.8x) | 2.64 (1.8x) |
| `new` and free (`.hot` only) | 10.4 | 56.1 (5.4x) | 61.1 (5.9x) | 58.6 (5.6x) |

The string row is nearly the same in the three backends: `.hot` 0.80x to 0.85x and `.cpp` 0.94x to 0.97x.

## How to read the ratios

- 1.0x is native speed. A ratio below 1.0x is noise or a small difference in the code that the compiler makes.
- The three backends give the same code speed. They differ in the time of a load, not in the time of a run.
- A loop of fields, numbers, arrays and calls inside one script runs at native speed. This is true for `.hot` and for `.cpp`.
- A call from the host into a script costs 3.5 to 4.4 ns more than a native virtual call, in `.hot` and in `.cpp`. The call goes through a slot that a reload can change, and it catches the script errors.
- A `.cpp` script is as fast as a `.hot` script, or faster, in most cases. It is faster in three cases: `std::vector` (no check), the closure and the string.
- The `.hot` closure call costs 2.5x. The `.cpp` lambda in a `std::function` runs at native speed.
- The script to script call is 1.0x to 1.2x in both languages. The difference between the two is of the size of the run noise.
- A ratio applies to the case only, not to a whole program. A ratio of 3.4x on a call of 1.5 ns is 3.5 ns more for each call.

## The cost of `new`

The `.hot` case makes an object with `new`, writes a field and frees the object. A script object has two parts. The shell is the object that native code sees. The state block holds the fields, and a reload can replace it. A `std::shared_ptr` owns the shell. So one `new` makes three blocks of memory. The native twins do the same work with other owners. A `.cpp` script has no such object, so this case has no `.cpp` script.

| Native owner | native | `files` | `jit` | `aot` |
|---|---|---|---|---|
| `std::make_shared<T>()` | 10.4 | 5.4x | 5.9x | 5.6x |
| `std::shared_ptr<T>(new T)` | 17.6 | 3.2x | 3.5x | 3.3x |
| `std::make_unique<T>()` | 7.9 | 7.1x | 7.7x | 7.4x |
| intrusive pointer (atomic count) | 14.1 | 4.0x | 4.3x | 4.2x |
| virtual base + state block, `shared_ptr(new T)` | 38.7 | 1.4x | 1.6x | 1.5x |

The last row has the shape of a script object. It is a class with a virtual base. Its constructor allocates a state block and runs the field initializers through a table. A `std::shared_ptr` made from `new` owns it. The script is about 1.5x of this twin. The difference is the work that a reload needs. This is the list of live objects and the constructor lookup. A profile shows that `malloc` and `free` take about half of the script time. The native time of the last row changed by 12% between runs, so its ratios are less sure.

The earlier version used a hash map for the list of live objects. Then `new` and free took 100 ns in `aot`. Now the list is in the object, and a small state block uses `malloc`. The time is about 60 ns.

## Reloadable access and calls

These cases use code that a reload can change. The twin is the same code in plain C++. It uses a direct member access, a plain virtual call and a plain call. Time in ns for one operation, `aot` backend.

| Case | native | `.hot` | `.cpp` |
|---|---|---|---|
| field of another object (through a handle) | 0.58 | 0.61 (1.1x) | n/a |
| small method call on `this` | 1.46 | 1.46 (1.0x) | 1.46 (1.0x) |
| method call through a handle | 1.46 | 1.46 (1.0x) | n/a |
| script to script virtual call | 1.46 | 2.64 (1.8x) | n/a |
| module variable read and write | 0.38 | 0.36 (0.94x) | 0.36 (0.94x) |
| module function and static method | 0.32 | n/a | 0.41 (1.3x) |

- A field access through a handle and a method call stay at native speed. The compiler inlines the method and moves the generation check out of the loop.
- A virtual call from script to script reads a slot. It costs 0.7 to 1.2 ns more than a native virtual call, depending on the backend.
- The `.hot` language has no free functions and no static methods. The case uses a `.cpp` script. The native twin of the case is a free function and a static method.

### A call after N reloads

The host calls a script override (`Hook`) that reads a field. The object is made in generation 1 and the tool reloads the script N times. Each reload adds or removes a field, so the state of the object migrates. The native virtual call takes 1.46 ns. The case uses a `.hot` script.

| N | `files` | `jit` |
|---|---|---|
| 0 | 5.55 (3.8x) | 5.55 (3.8x) |
| 1 | 5.55 (3.8x) | 5.55 (3.8x) |
| 10 | 5.55 (3.8x) | 5.55 (3.8x) |
| 100 | 5.55 (3.8x) | 5.55 (3.8x) |

The cost does not grow with the number of reloads. A new object, made after the last reload, gives the same time (5.55 ns, and 5.57 ns at N = 10 in `jit`). The `aot` backend has no reload.

The tool also measures one reload of 100 objects with 65 fields, without the compile. It takes 0.10 ms in `files` and 1.4 ms in `jit`.

## Caveats

- The cases are small loops. A real program has other costs: cache misses, the size of the code and the calls between modules.
- A twin is only as fair as its code. The string twin uses `std::to_string`. The `.hot` script uses `${}` (fmt). The `.cpp` script uses `std::to_string`, as the twin does. So the `.hot` string row is a rough value.
- A `.cpp` script is a normal C++ file, and the compiler sees all of it. The native twin hides some values from the compiler (for the string, the closure and `new`). So a ratio of a `.cpp` script below 1.0x can come from this difference.
- libstdc++ uses plain (not atomic) `std::shared_ptr` counts while a process has only one thread. The tool starts a thread first, so all cases use the atomic counts, as in a host with threads.
- The compiler can move work out of a loop in one case and not in its twin. The cases use a value that each iteration needs, but a ratio near 1.0x can still hide a small cost.
- `HOT_RUNTIME_CHECKS=OFF` removes the null and index checks of `.hot` code. Build again with that option to compare. The tool prints the value of the option. A `.cpp` script has no such checks.
- The reload cases use `.hot` scripts only.
- One machine and one compiler. Measure again on your own machine and with your own headers.
