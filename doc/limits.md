---
layout: default
title: Limits
nav_order: 14
---

This list shows what version 1 does not support.

- Coroutines.
- Generics in script code. You can use C++ templates.
- Script exception types. You can throw and catch C++ types.
- The `?.` operator.
- Reload while a worker thread runs script code. The default commit waits for the frame boundary.
- A stored `ref`, `T&` or `T*`. A borrowed value cannot go to a field. The option `--no-strict-borrowed` makes this a warning.
- Free functions. Only the `convert_` hooks can stand at file level.
- Brace initializers. Write `T(args)`.
- `::`, `->` and C style casts.
- Member access with `.` on a type with `operator->`.
- Windows and macOS. They are untested.
- MSVC, Visual Studio, and a real gdb or lldb session. They are untested.
