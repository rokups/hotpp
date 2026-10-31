---
layout: default
title: Home
nav_order: 0
---

Hot++ is a scripting language for C++ programs. A script is a `.hot` file that looks like C#.
The `hotc` tool turns a script into C++. The engine reloads a changed script while it runs.

- [Introduction](intro.html): what Hot++ is, a `.hot` and a `.cpp` script side by side, and how `hotc` makes the features work.
- [Getting started](getting-started.html): write a class, check it and compile it.

## Hot script

- [Types](types.html): builtin types, collection literals, handles, references and pointers.
- [Borrowing](borrowing.html): where `T&`, `T*` and `&x` are allowed, and the errors.
- [Classes](classes.html): fields, methods, `=>` bodies, default values, constructors, properties, structs, access words.
- [Inheritance](inheritance.html): native bases, virtual methods, script parents.
- [Module level](module-level.html): `using`, enums, structs, module variables, native stubs.
- [Statements](statements.html): control flow, string `switch`, `foreach` and bindings, `try`, string interpolation, casts, `??`, `?.`, `is` and `as`.
- [Lambdas](lambdas.html): arrow functions with a block body or an expression body.
- [Native interop](native-interop.html): how a script calls engine code directly.
- [Limits](limits.html): what version 1 does not support.

## More

- [Hot reload](hot-reload.html): how a reload swaps the code of a running object.
- [C++ scripts](cpp-scripts.html): a script as a plain `.cpp` file that also compiles in a normal build.
- [Embedding](embedding.html): load scripts with a `Loader`.
- [CMake](cmake.html): use Hot++ in a host project.
- [Tools](tools.html): the `hot-ls` language server.
- [Diagnostics](diagnostics.html): the error format of the tools.
