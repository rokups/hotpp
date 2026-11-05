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
- An exception struct (a struct with the base `ScriptError`) as the type of a field, a module variable or an element of a container. Use it only for `throw` and `catch`. A `.cpp` script has no exception struct. The only base of an exception struct is `ScriptError` (or another exception struct). See [Script exception types](statements-errors.html#script-exception-types).
- Reload while a worker thread runs script code, in the default `StopTheWorld` mode. The commit waits for the frame boundary. The `EntryCounters` mode allows it (see [Internals](internals.html)).
- A stored `ref` parameter, `ref` local or `T*`. A borrowed value cannot go to a field. The option `--no-strict-borrowed` makes this a warning for `T*`. A stored `ref` is always an error. See [Borrowing](borrowing.html).
- The check of a `ref` (see "Lifetime of a ref" in [Type design](type-design.html#lifetime-of-a-ref)) has these limits:
  - It sees direct mutation only: a call of a method name from the shape list, an assignment, `&c`, and a bare name passed to a `ref` or `out` parameter.
  - It does not check a callee with 3 or more name parts, such as `p.items.push_back(x)`.
  - A `ref` into a field of another object with 3 or more name parts, such as `ref r = a.b.c`, uses the root `a` only.
  - A `var` local or a `ref` local of an unknown type counts as a handle. `var v = Vec3(); ref r = v.x; v = Vec3();` is a false error.
  - A field of a type that is not a builtin scalar (a struct, an enum, a native type) counts as able to change shape after a `ref` call. This can give a false error.
  - After a call of a `ref` method, a shape change of any reshapeable field ends the `ref`, even a field that the method does not touch. This is a false error.
  - The message for a replaced owner says "elements of" the owner, also when the owner is a handle or a `T?`.
  - It does not see mutation by another method of the same class, `std.swap(a, b)`, or a free function that takes the container.
  - A native `T&` getter cannot bind to a `ref`. Copy the value, or use an index.
  - A `ref` call on an object of another class counts as a temporary.
  - `ref r = xs[await Idx()]` is allowed, because the ref starts after its own statement. Another live ref gives the error about that ref.
  - `foreach (var x in c)` with a change of `c` in the body is not checked yet.
  - A `.cpp` script keeps C++ semantics and has no `ref` lifetime rules.
- A script class, struct or enum with the name of a C++ type from a `using` header. Rename the script type.
- Shared state in an `inline` function, an `inline` variable or a template static member of a host header. The script modules build with hidden symbol visibility. Each module gets its own copy of that state, and the host does not share it. Put such state in a `.cpp` file of the host, or mark it exported.
- A namespace block in a `.hot` script. Use `using ns;` for a native namespace.
- A call of a free function by name from the host through `Loader::thunk`. Call a method of a class.
- A function as a value, such as `var f = Add;`. Call it, or write a lambda.
- An `async` free function or an `async` static method. An async function needs an object.
- A collection literal without a declared type: `var x = [1, 2];` is an error.
- A `.cpp` script has no null or bounds checks. The translator keeps a `.cpp` body as C++ text and does not parse expressions. The checks cover `.hot` scripts only.
- A bounds check covers a C array, `std::array`, `std::vector`, `std::deque`, `std::basic_string` (`std::string`), `std::basic_string_view`, `std::span`, `hot::span` and a stub with `[native, checked_index]`. `std::map` and `std::unordered_map` keep their own `operator[]`, which inserts a key. Use `at` or `find`. Other C++ containers have no bounds check, because the translator does not know their index rules.
- A `ScriptError` in a constructor goes to the code that creates the object. A script `new` can catch it. A host `create` call gets the C++ exception. The script error handler does not see it, because no object exists yet.
- A class with two constructors of the same arity does not coerce `null` for a `[weak]` parameter. Give the constructors different arities. The translator cannot choose the constructor from a `null` argument.
- A `noexcept` native virtual that a script overrides and whose result has no default value (a reference, for example). The host must give a `hot::noexcept_result<R>` specialization for it. Without it an unhandled `ScriptError` ends the program.
- A `noexcept` pure virtual native method that no script class implements. The call traps: it reports `abstract method 'C.F' called` and does not end the program.
- Script interfaces (see [Interfaces](inheritance.html#interfaces)) have these limits. An interface cannot inherit from another interface. It has no generics, no fields, no default methods, no `static` members and no `async` methods. A struct cannot implement an interface. `[weak]` does not work with an interface, and `new` of an interface is an error. A `.cpp` script has no interfaces. The optimized AOT mode does not support them.
- Script operators (see [Operators](classes.html#operators)) have these limits. There are no `static` operators, no `<=>`, no conversion operators and no overloads. An interface and a `[native]` stub cannot have an operator. A `.cpp` script has no script operators. An operator on a handle is an error, except `==` and `!=`.
- A comparison or an operator with a value return cannot write a field of its own type. The checker reports a direct write (`x = 1`, `x++`, `this.x = 1`, a write to a stored array or container) and a call of a function of the type that writes a field. It does not see a write through a field of a struct type (`pos.x = 1`), a write to a field of a script parent, a call of a native member function on a field, or a write through a handle. The generated member keeps `const_cast`, so such a write is wrong for an object that is truly `const`.
- A compound operator (`+=`) is not available on a subclass when the binary operator comes from the parent. `assign_value` throws a `hot::ScriptError` when the two objects have different classes.
- A reload that changes the signature of an interface method on one side only. The translator reports an error and the old generation stays active.
- A native virtual method that a script overrides implements an interface method only when the C++ types are equal.
- Access checks on native members. Only members of known script types are checked, because the translator has no member list for other types. The C++ compiler reports a private native member. The message names the C++ member and points at the script line.
- Two classes whose field initializers build each other with `new`. No order of the classes works, so `hotc` reports `class '<name>' must come before the class that builds it with 'new'`. Use a constructor or a method for one of the two. A field initializer can use a class that comes later in the file when there is no such cycle.
- Brace initializers. Write `T(args)`.
- `::` and `->`. Write `.`.
- A C style cast to a type other than a builtin type or `T*`. Use `static_cast<T>(e)`.
- Member access with `.` on an iterator, a `std.optional` or a `std.unique_ptr`. Write `(*p).x`.
- A field of a move-only type, such as `std.unique_ptr<T>`. A reload copies each field. Use a handle.
- More than 64 nested statements, 15 nested lambdas or 256 operators in one expression. Each level uses parser stack, so the limits are fixed. The parser reports `maximum nesting depth exceeded`.
- macOS. It is untested. Windows works with MSVC, and the CI tests it on Windows (see [Windows](windows.html)).
- A real Visual Studio or gdb debugger session. It is untested. An lldb session and an lldb-dap session run in the tests on Linux when lldb is installed.

A `.cpp` script (see [C++ scripts](cpp-scripts.html)) has more limits. It has no properties, no script virtual methods without `override`, no script-class handles and no `new`, and no script inheritance.

## Limits

These limits come from the type system and the null analysis.

- A member-level `[nonnull]` is not available in a stub. A stub has no members. Use `[nonnull]` on a whole class.
- The analyzer does not check an argument to a `[nonnull]` parameter.
- `h.reset(rawptr)` is always an error. A shared pointer cannot come from a raw pointer.
- `$NativeBase b = this;` is an error, unless `enable_shared_from_this` is known for the native base.
- `T[]` is `hot::span<T>` and works with C++17. The option `HOT_VIEW_TYPE=std` selects `std::span<T>`, which needs C++20. The choice is one constant for the whole build. See [The view type](types.html#the-view-type).
