---
layout: default
title: Benchmarks
nav_order: 43
---

The tool `bench_backends` measures the run-time cost of script code. Each case is a script and a native C++ twin that does the same work. The twin is the reference. The result is a ratio: script time divided by native time.

## How to run

Build the tool in a Release build. A build with no `CMAKE_BUILD_TYPE` compiles the runtime without optimization, and the ratios are larger. The tool is part of the build when `HOT_BUILD_TESTS` is `ON`.

    cmake -S . -B build-rel -G Ninja -DCMAKE_BUILD_TYPE=Release -DHOT_BUILD_TESTS=ON
    cmake --build build-rel --target bench_backends
    build-rel/bin/bench_backends --no-load

One binary tests every backend of the build:

- `files`: the clang files backend compiles each script at run time (`-O2`).
- `jit`: the JIT backend compiles each script in the process (`-O2`).
- `aot`: the scripts are C++ in the binary (`MODE AOT_RELOAD` of `hot_add_host`, `-O2`).

The native twins also use `-O2`. A backend that the build does not have is skipped.

| Option | Effect |
|---|---|
| `--backend all\|files\|jit\|aot` | test one backend only |
| `--filter text` | run only the cases whose name contains the text |
| `--reps N` | timed runs of each case; the result is the median (default 7) |
| `--iters N` | loop count of each case |
| `--reloads N` | the largest N of the reload chain (default 100) |
| `--csv file` | write the table also as CSV |
| `--no-load` | skip the load section (translate and compile time) |
| `--list` | print the backends and the cases |

The tool checks that each script gives the same result as its twin. A `!` after a time marks a different result.

Run the tool alone, on a quiet machine. Other work on the machine changes the numbers. One run of all backends takes about 4 minutes. Each case and each reload needs one compile in `files` and in `jit`. Pin the process to one core to get less noise: `taskset -c 6 build-rel/bin/bench_backends --no-load`.

## Machine

The CPU is an AMD Ryzen 9 3950X (a desktop CPU, 16 cores, 32 threads). The software is Linux 7.2, glibc 2.44, clang 23.1.3 and libstdc++. The build uses `CMAKE_BUILD_TYPE=Release`, `HOT_RUNTIME_CHECKS=ON` and `HOT_TRACEBACK=OFF`. The values are the median of 3 runs of the tool (each run gives the median of 7 timed runs). Between the 3 runs, most rows changed by less than 2%. These rows changed more:

- the array index in `files` and `aot` (one slow run, up to 44%)
- the script to native call in `jit` (15%)
- the script to script call in `files` (13%)
- `new` and free (3% to 9%)
- the reload in `files` (22%)

## Results

Time in ns for one operation. The ratio is in parentheses.

| Case | native | `files` | `jit` | `aot` |
|---|---|---|---|---|
| empty method call (host to script) | 1.46 | 5.26 (3.6x) | 5.56 (3.8x) | 4.96 (3.4x) |
| native to script virtual call | 1.46 | 5.26 (3.6x) | 5.55 (3.8x) | 4.98 (3.4x) |
| script to script call (recursive) | 1.23 | 1.21 (1.0x) | 1.22 (1.0x) | 1.20 (1.0x) |
| script to native call | 1.46 | 2.04 (1.4x) | 2.04 (1.4x) | 1.46 (1.0x) |
| field read and write loop | 0.88 | 0.88 (1.0x) | 0.88 (1.0x) | 0.88 (1.0x) |
| integer arithmetic loop | 1.41 | 1.33 (0.94x) | 1.41 (1.0x) | 1.41 (1.0x) |
| float arithmetic loop | 1.75 | 1.75 (1.0x) | 1.75 (1.0x) | 1.75 (1.0x) |
| array index (`int[256]`, checked) | 0.62 | 0.65 (1.0x) | 0.64 (1.0x) | 0.64 (1.0x) |
| `std.vector` index (checked) | 0.63 | 0.88 (1.4x) | 0.88 (1.4x) | 0.88 (1.4x) |
| handle dereference and null check | 9.85 | 10.5 (1.1x) | 10.4 (1.1x) | 10.4 (1.1x) |
| string interpolation | 67.7 | 54.3 (0.8x) | 58.1 (0.9x) | 53.7 (0.8x) |
| `new` and free of a script object | 10.4 | 55.6 (5.3x) | 56.9 (5.5x) | 59.0 (5.7x) |
| closure call | 1.75 | 4.39 (2.5x) | 4.41 (2.5x) | 4.38 (2.5x) |
| loop with a method call | 1.26 | 1.25 (1.0x) | 1.25 (1.0x) | 1.25 (1.0x) |

## How to read the ratios

- 1.0x is native speed. A ratio below 1.0x is noise or a small difference in the code that the compiler makes.
- The three backends give the same code speed. They differ in the time of a load, not in the time of a run.
- A loop of fields, numbers, arrays and calls inside one script runs at native speed.
- A call from the host into a script costs about 3.5 ns more than a native virtual call. The call goes through a slot that a reload can change, and it catches the script errors.
- A ratio applies to the case only, not to a whole program. A ratio of 3.5x on a call of 1.5 ns is 3.5 ns more for each call.

## The cost of `new`

The script case makes an object with `new`, writes a field and frees the object. A script object has two parts. The shell is the object that native code sees. The state block holds the fields, and a reload can replace it. A `std::shared_ptr` owns the shell. So one `new` makes three blocks of memory. The native twins do the same work with other owners.

| Native owner | native | `files` | `jit` | `aot` |
|---|---|---|---|---|
| `std::make_shared<T>()` | 10.4 | 5.3x | 5.5x | 5.7x |
| `std::shared_ptr<T>(new T)` | 17.6 | 3.2x | 3.2x | 3.4x |
| `std::make_unique<T>()` | 7.9 | 7.0x | 7.2x | 7.5x |
| intrusive pointer (atomic count) | 14.1 | 3.9x | 4.0x | 4.2x |
| virtual base + state block, `shared_ptr(new T)` | 43.3 | 1.3x | 1.3x | 1.4x |

The last row has the shape of a script object. It is a class with a virtual base. Its constructor allocates a state block and runs the field initializers through a table. A `std::shared_ptr` made from `new` owns it. The script is about 1.3x of this twin. The difference is the work that a reload needs. This is the list of live objects and the constructor lookup. A profile shows that `malloc` and `free` take about half of the script time.

The earlier version used a hash map for the list of live objects. Then `new` and free took 100 ns in `aot`. Now the list is in the object, and a small state block uses `malloc`. The time is about 60 ns.

## Reloadable access and calls

These cases use code that a reload can change. The twin is the same code in plain C++. It uses a direct member access, a plain virtual call and a plain call.

| Case | native | `files` | `jit` | `aot` |
|---|---|---|---|---|
| field of another object (through a handle) | 0.58 | 0.58 (1.0x) | 0.58 (1.0x) | 0.62 (1.1x) |
| small method call on `this` | 1.46 | 1.46 (1.0x) | 1.46 (1.0x) | 1.46 (1.0x) |
| method call through a handle | 1.46 | 1.46 (1.0x) | 1.46 (1.0x) | 1.46 (1.0x) |
| script to script virtual call | 1.46 | 2.16 (1.5x) | 2.34 (1.6x) | 2.34 (1.6x) |
| module variable read and write | 0.38 | 0.36 (0.9x) | 0.36 (0.9x) | 0.36 (0.9x) |
| module function and static method (`.cpp` script) | 0.32 | 0.41 (1.3x) | 0.41 (1.3x) | 0.41 (1.3x) |

- A field access through a handle and a method call stay at native speed. The compiler inlines the method and moves the generation check out of the loop.
- A virtual call from script to script reads a slot. It costs about 0.8 ns more than a native virtual call.
- The `.hot` language has no free functions and no static methods. The case uses a `.cpp` script.

### A call after N reloads

The host calls a script override (`Hook`) that reads a field. The object is made in generation 1 and the tool reloads the script N times. Each reload adds or removes a field, so the state of the object migrates. The native virtual call takes 1.46 ns.

| N | `files` | `jit` |
|---|---|---|
| 0 | 5.26 (3.6x) | 5.26 (3.6x) |
| 1 | 5.26 (3.6x) | 5.27 (3.6x) |
| 10 | 5.27 (3.6x) | 5.26 (3.6x) |
| 100 | 5.26 (3.6x) | 5.26 (3.6x) |

The cost does not grow with the number of reloads. A new object, made after the last reload, gives the same time. The `aot` backend has no reload.

The tool also measures one reload of 100 objects with 65 fields, without the compile. It takes 0.10 ms in `files` and 1.2 ms in `jit`.

## Caveats

- The cases are small loops. A real program has other costs: cache misses, the size of the code and the calls between modules.
- A twin is only as fair as its code. The string twin uses `std::to_string` and the script uses `${}` (fmt), so that row is a rough value.
- libstdc++ uses plain (not atomic) `std::shared_ptr` counts while a process has only one thread. The tool starts a thread first, so all cases use the atomic counts, as in a host with threads.
- The compiler can move work out of a loop in one case and not in its twin. The cases use a value that each iteration needs, but a ratio near 1.0x can still hide a small cost.
- `HOT_RUNTIME_CHECKS=OFF` removes the null and index checks. Build again with that option to compare. The tool prints the value of the option.
- One machine and one compiler. Measure again on your own machine and with your own headers.
