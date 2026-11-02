---
layout: default
title: Limits
nav_order: 21
group: Hot script
---

This list shows what version 1 does not support.

- The words `static`, `thread_local`, `friend`, `mutable` and `volatile` on a member of a `.hot` class. For a `static` member, use a module variable (a variable at file level).
- Coroutines other than `async` methods. See [Async and await](async.html).
- Generics in script code. You can use C++ templates.
- Script exception types. You can throw and catch C++ types.
- The `?.` operator inside an expression. It works only at the start of an expression statement.
- Reload while a worker thread runs script code. The default commit waits for the frame boundary.
- A stored `T&` or `T*`. A borrowed value cannot go to a field. The option `--no-strict-borrowed` makes this a warning. See [Borrowing](borrowing.html).
- A script class, struct or enum with the name of a C++ type from a `using` header. Rename the script type.
- Shared state in an `inline` function, an `inline` variable or a template static member of a host header. The script modules build with hidden symbol visibility. Each module gets its own copy of that state, and the host does not share it. Put such state in a `.cpp` file of the host, or mark it exported.
- Free functions. Only the `convert_` hooks can stand at file level.
- A collection literal without a declared type: `var x = [1, 2];` is an error.
- Runtime checks (null and bounds) in `.cpp` scripts. They cover `.hot` scripts only. They also skip a C++ container other than `std::array` and a C array, and they skip a throw in a constructor.
- A `noexcept` native virtual that a script overrides and whose result has no default value (a reference, for example). An unhandled `ScriptError` there ends the program.
- A `noexcept` pure virtual native method that no script class implements. The call traps: it reports `abstract method 'C.F' called` and does not end the program.
- Access checks on native members. Only members of known script types are checked.
- Brace initializers. Write `T(args)`.
- `::` and `->`. Write `.`.
- A C style cast to a type other than a builtin type, `T*` or `T&`. Use `static_cast<T>(e)`.
- Member access with `.` on an iterator, a `std.optional` or a `std.unique_ptr`. Write `(*p).x`.
- A field of a move-only type, such as `std.unique_ptr<T>`. A reload copies each field. Use a handle.
- More than 64 nested statements, 15 nested lambdas or 256 operators in one expression.
- macOS. It is untested. Windows works with MSVC, and the CI tests it on Windows (see [Windows](windows.html)).
- A real Visual Studio, gdb or lldb debugger session. It is untested.

A `.cpp` script (see [C++ scripts](cpp-scripts.html)) has more limits. It has no properties, no script virtual methods without `override`, no script-class handles and no `new`, and no script inheritance.
