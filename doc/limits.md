---
layout: default
title: Limits
nav_order: 16
---

This list shows what version 1 does not support.

- Coroutines.
- Generics in script code. You can use C++ templates.
- Script exception types. You can throw and catch C++ types.
- The `?.` operator inside an expression. It works only at the start of an expression statement.
- Reload while a worker thread runs script code. The default commit waits for the frame boundary.
- A stored `T&` or `T*`. A borrowed value cannot go to a field. The option `--no-strict-borrowed` makes this a warning. See [Borrowing](borrowing.html).
- Free functions. Only the `convert_` hooks can stand at file level.
- A collection literal without a declared type: `var x = [1, 2];` is an error.
- Access checks on native members. Only members of known script types are checked.
- Brace initializers. Write `T(args)`.
- `::` and `->`. Write `.`.
- A C style cast to a type other than a builtin type, `T*` or `T&`. Use `static_cast<T>(e)`.
- Member access with `.` on an iterator, a `std.optional` or a `std.unique_ptr`. Write `(*p).x`.
- A field of a move-only type, such as `std.unique_ptr<T>`. A reload copies each field. Use a handle.
- Windows and macOS. They are untested.
- MSVC, Visual Studio, and a real gdb or lldb session. They are untested.
