---
layout: default
title: C++ scripts
nav_order: 22
group: C++ scripts
---

A `.cpp` script is a plain C++ file. It uses the same hot reload as a `.hot` script. This page is the overview of the `.cpp` script mode.

## What a `.cpp` script is

A `.cpp` script holds a class that derives from a native engine class. The class has fields and methods. The host gives the path of the file to the engine, as it does for a `.hot` script.

The extension selects the parser:

- `.hot` selects the custom Hot++ syntax.
- `.cpp` selects libclang.

Both modes work in one program. A `Loader` holds one module for each generation, as for `.hot` scripts. Use one session for each script. There is no import between the modes. A `.cpp` script cannot include a `.hot` script. A `.hot` script cannot import a `.cpp` script.

## The file compiles unchanged

A normal CMake build compiles a `.cpp` script with no `hotc`. Do not write `#include "hot/script.h"` in the script. The tool `hotc` injects `hot/script.h` itself. The header gives the macro `HOT_ALIAS` and the type `hot::OldState`.

The aids `HOT_ALIAS`, `onReload` and `convert_*` are temporary. Use them for a reload session. Remove them after the session. Then the file is standard C++ and a normal build compiles it with no extra option. The fixture `tests/scripts/cpp_player_v1.cpp` has no aids.

A normal build of a file that still has aids needs the header. Pass `-include hot/script.h` to the compiler, and add the `include` directory of Hot++ to the include path. The fixture `tests/scripts/cpp_player_v2.cpp` has aids. Its plain target `cpp_scripts_plain2` does this.

In a normal build the file has plain C++ meaning. There is no state migration and there are no slots. Do not share objects between the two builds.

## Pages of this section

- [Example](cpp-example.html): two generations of a `.cpp` script, and the CMake commands that build them.
- [How hotc edits C++](cpp-translation.html): the edits of a method body, lambdas and namespaces.
- [Script classes](cpp-classes.html): what a class can have, static members, definitions outside the class, member initializer lists, aliases and hooks.
- [Types and inheritance](cpp-types.html): a script class as a type, casts, and script class inheritance.
- [Limits and errors](cpp-limits.html): the unsupported forms with the messages of `hotc`, and the known limits.
