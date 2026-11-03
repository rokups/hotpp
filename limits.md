---
layout: default
title: Limits
nav_order: 21
group: Hot script
---

This list shows what version 1 does not support.

- The words `thread_local`, `friend`, `mutable` and `volatile` on a member of a `.hot` class. A `static` method works (see [Static methods](classes.html#static-methods)). A `static` field does not. Use a module variable (a variable at file level).
- Coroutines other than `async` methods. See [Async and await](async.html).
- Generics in script code. You can use C++ templates.
- Script exception types. You can throw and catch C++ types.
- Reload while a worker thread runs script code. The default commit waits for the frame boundary.
- A stored `ref` parameter, `ref` local or `T*`. A borrowed value cannot go to a field. The option `--no-strict-borrowed` makes this a warning. See [Borrowing](borrowing.html).
- A script class, struct or enum with the name of a C++ type from a `using` header. Rename the script type.
- Shared state in an `inline` function, an `inline` variable or a template static member of a host header. The script modules build with hidden symbol visibility. Each module gets its own copy of that state, and the host does not share it. Put such state in a `.cpp` file of the host, or mark it exported.
- A namespace block in a `.hot` script. Use `using ns;` for a native namespace.
- A call of a free function by name from the host through `Loader::thunk`. Call a method of a class.
- A function as a value, such as `var f = Add;`. Call it, or write a lambda.
- An `async` free function or an `async` static method. An async function needs an object.
- A collection literal without a declared type: `var x = [1, 2];` is an error.
- Runtime checks (null and bounds) in `.cpp` scripts. They cover `.hot` scripts only. They also skip a C++ container other than `std::array` and a C array, and they skip a throw in a constructor.
- A `noexcept` native virtual that a script overrides and whose result has no default value (a reference, for example). An unhandled `ScriptError` there ends the program.
- A `noexcept` pure virtual native method that no script class implements. The call traps: it reports `abstract method 'C.F' called` and does not end the program.
- Script interfaces (see [Interfaces](inheritance.html#interfaces)) have these limits. An interface cannot inherit from another interface. It has no generics, no fields, no default methods, no `static` members and no `async` methods. A struct cannot implement an interface. `[weak]` does not work with an interface, and `new` of an interface is an error. A `.cpp` script has no interfaces. The optimized AOT mode does not support them.
- Script operators (see [Operators](classes.html#operators)) have these limits. There are no `static` operators, no `<=>`, no conversion operators and no overloads. An interface and a `[native]` stub cannot have an operator. A `.cpp` script has no script operators. An operator on a handle is an error, except `==` and `!=`.
- A comparison or an operator with a value return cannot write a field of its own type. The checker reports a direct write (`x = 1`, `x++`, `this.x = 1`, a write to a stored array or container) and a call of a function of the type that writes a field. It does not see a write through a field of a struct type (`pos.x = 1`), a write to a field of a script parent, a call of a native member function on a field, or a write through a handle. The generated member keeps `const_cast`, so such a write is wrong for an object that is truly `const`.
- A compound operator (`+=`) is not available on a subclass when the binary operator comes from the parent. `assign_value` throws a `hot::ScriptError` when the two objects have different classes.
- A reload that changes the signature of an interface method on one side only. The translator reports an error and the old generation stays active.
- A native virtual method that a script overrides implements an interface method only when the C++ types are equal.
- Access checks on native members. Only members of known script types are checked.
- Brace initializers. Write `T(args)`.
- `::` and `->`. Write `.`.
- A C style cast to a type other than a builtin type or `T*`. Use `static_cast<T>(e)`.
- Member access with `.` on an iterator, a `std.optional` or a `std.unique_ptr`. Write `(*p).x`.
- A field of a move-only type, such as `std.unique_ptr<T>`. A reload copies each field. Use a handle.
- More than 64 nested statements, 15 nested lambdas or 256 operators in one expression.
- macOS. It is untested. Windows works with MSVC, and the CI tests it on Windows (see [Windows](windows.html)).
- A real Visual Studio, gdb or lldb debugger session. It is untested.

A `.cpp` script (see [C++ scripts](cpp-scripts.html)) has more limits. It has no properties, no script virtual methods without `override`, no script-class handles and no `new`, and no script inheritance.

## Limits

These limits come from the type system and the null analysis.

- A member-level `[nonnull]` is not available in a stub. A stub has no members. Use `[nonnull]` on a whole class.
- The analyzer does not check an argument to a `[nonnull]` parameter.
- `h.reset(rawptr)` is always an error. A shared pointer cannot come from a raw pointer.
- `$NativeBase b = this;` is an error, unless `enable_shared_from_this` is known for the native base.
- `T[]` needs C++20, because it is `std::span<T>`.
