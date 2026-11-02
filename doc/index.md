---
layout: default
title: Home
nav_order: 0
---

Hot++ is a scripting language for C++ programs. A script is a `.hot` file that looks like C#.
The `hotc` tool turns a script into C++. The engine reloads a changed script while it runs.

- [Introduction](intro.html): what Hot++ is, a `.hot` and a `.cpp` script side by side, and how `hotc` makes the features work.
- [Getting started](getting-started.html): write a class, check it and compile it.

- [Hot reload](hot-reload.html): how a reload swaps the code of a running object.

## Hot script

- [Types](types.html): builtin types, variables, C++ types, collection literals and arrays.
  - [Handles, references and pointers](types-handles.html): handles, weak handles, value uses (`[value]`), references and pointers.
- [Borrowing](borrowing.html): where `T&`, `T*` and `&x` are allowed or rejected, and handles.
  - [Borrowed locals, calls and fixes](borrowing-locals.html): borrowed locals, borrowed values in calls, the escape hatch and quick fixes.
- [Classes](classes.html): fields, methods, `=>` bodies, default values, constructors, properties, structs.
  - [Mutual use, this and access words](classes-use.html): classes that use each other, `this` as a value, access words and renamed fields.
- [Inheritance](inheritance.html): native bases, virtual methods, script parents.
- [Module level](module-level.html): `using`, enums, structs, module variables, native stubs.
- [Statements](statements.html): control flow, string `switch`, `foreach` and structured bindings.
  - [try, catch and runtime errors](statements-errors.html): `try`, `catch`, `finally` and the errors that the runtime throws.
  - [Strings, casts and patterns](statements-text.html): string interpolation, casts, `??`, `?.`, `is` and `as`.
- [Lambdas](lambdas.html): arrow functions with a block body or an expression body.
- [Native interop](native-interop.html): how a script calls engine code directly, native objects and the fallback handle.
  - [Scope of a stub and C++ insertions](native-interop-scope.html): the scope of a stub and the C++ insertions.
  - [operator->, stub checks and index bounds](native-interop-checks.html): types with `operator->`, the checks on a stub and the bounds check on an index.
  - [Module attributes and other limits](native-interop-attributes.html): module attributes, the array type and other limits.
- [Diagnostics](diagnostics.html): the error format of the tools.
- [Limits](limits.html): what version 1 does not support.

## C++ scripts

- [C++ scripts](cpp-scripts.html): a script as a plain `.cpp` file that also compiles in a normal build.
- [Example](cpp-example.html): two generations of a `.cpp` script and the CMake commands that build them.
- [How hotc edits C++](cpp-translation.html): the edits of a method body, lambdas and namespaces.
- [Script classes](cpp-classes.html): what a class can have, static members, definitions outside the class, member initializer lists.
- [Types and inheritance](cpp-types.html): a script class as a type, casts and script class inheritance.
- [Limits and errors](cpp-limits.html): the unsupported forms with the messages of `hotc`, and the known limits.

## More

- [Embedding](embedding.html): load scripts with a `Loader`, and which hosting situation fits.
  - [Rules, errors and traceback](embedding-rules.html): ownership, threads, symbol export, error reports and the traceback.
  - [Run-time compile and precompiled library](embedding-runtime.html): the two situations that load a library.
  - [Reload API](embedding-reload.html): `request_reload`, the status values and the full reload.
  - [JIT, clang files and AOT](embedding-backends.html): the JIT, the clang files backend, AOT and AOT with overrides.
  - [File system of the host](embedding-vfs.html): the virtual file system for imports and headers.
  - [Backend interface and options](embedding-interface.html): the backend interface and the options structs.
- [CMake](cmake.html): use Hot++ in a host project, and the quick start.
  - [Setup and targets](cmake-setup.html): the requirements, how to consume Hot++ and the targets.
  - [Options and standard](cmake-options.html): the build options, the C++ standard and the format library.
  - [Hosts with hot_add_host](cmake-host.html): the keywords and the modes.
  - [Script functions](cmake-scripts.html): `hot_add_scripts` and `hot_script_host`.
  - [SDK and portable binaries](cmake-sdk.html): install the SDK, and build a host without build paths.
  - [Presets, tests and troubleshooting](cmake-presets.html): sanitizers, presets, the table of errors and `hot_gtest`.
- [Tools](tools.html): the `hot-ls` language server.
- [Benchmarks](benchmarks.html): the run-time cost of script code against native C++, and how to measure it.
- [Development notes](development.html): the milestones, the source layout, the tests, the translator and the `hotc` command line.
- [Language server internals](tools-ls.html): the script hosts, the index and the features of `hot-ls`.
- [Runtime internals](internals.html): the JIT backend, threads, closures and debug information.

## License

Hot++ uses the MIT license. See [`LICENSE`](https://github.com/rokups/hotpp/blob/master/LICENSE).
