---
layout: default
title: Limits
nav_order: 21
group: Hot script
---

This list shows what version 1 does not support.

- The words `thread_local`, `friend`, `mutable` and `volatile` on a member of a `.hot` class. A `static` method works (see [Static methods](classes.html#static-methods)). A `static` field does not. Use a module variable (a variable at file level).
- Coroutines other than `async` methods. See [Async and await](async.html).
- Generic methods, generic interfaces, generic enums, constraints, type argument inference and the use of a generic of an imported unit. See [Generics](generics.html#not-supported).
- An exception struct (a struct with the base `ScriptError`) as the type of a field, a module variable or an element of a container. Use it only for `throw` and `catch`. A `.cpp` script has no exception struct. The only base of an exception struct is `ScriptError` (or another exception struct). See [Script exception types](statements-errors.html#script-exception-types).
- Reload while a worker thread runs script code, in the default `StopTheWorld` mode. The commit waits for the frame boundary. The `EntryCounters` mode allows it (see [Internals](internals.html)).
- A stored `ref` parameter, `ref` local or `T*`. A borrowed value cannot go to a field. The option `--no-strict-borrowed` makes this a warning for `T*`. A stored `ref` is always an error. See [Borrowing](borrowing.html).
- A `ref` with an element or an owner on its path is a checked path (rule R2 in [Type design](type-design.html#lifetime-of-a-ref)). It has these limits:
  - With `HOT_RUNTIME_CHECKS` off, the index of the path is not checked. A use after `clear()` is undefined behavior.
  - The path is evaluated at each use. A use costs one index check or one null check for each step.
- The static check of a `ref` call result (rule R5 in [Type design](type-design.html#lifetime-of-a-ref)) has these limits:
  - It sees direct mutation only: a call of a method name from the shape list, an assignment, and a bare name passed to a `ref` or `out` parameter.
  - It does not check a callee with 3 or more name parts, such as `p.items.push_back(x)`.
  - A `ref` into a field of another object with 3 or more name parts, such as `ref r = a.b.c`, uses the root `a` only.
  - A field of a type that is not a builtin scalar (a struct, an enum, a native type) counts as able to change shape after a `ref` call. This can give a false error.
  - After a call of a `ref` method, a shape change of any reshapeable field ends the `ref`, even a field that the method does not touch. This is a false error.
  - The message for a replaced owner says "elements of" the owner, also when the owner is a handle or a `T?`.
  - It does not see mutation by another method of the same class, `std.swap(a, b)`, or a free function that takes the container.
  - A native call on a native receiver (`ref m = o.GetMotion();`) gives a place that is an element of the receiver. The checker does not know the real owner of the reference. A receiver of an unknown type (a `var` local, a longer chain) counts as native. When the method returns a value, the C++ compile fails.
  - A `var p` from a `T*` return of a value struct is a borrowed pointer. A later shape change of the owner is not seen: `var p = c.Find(0); c.items.push_back(x); p.x = 1;` can use a freed element. A typed local `Info* p` of a script struct is not allowed: use `var`.
  - After `ref r = o.F();` on an object `o` of another script class, an assignment to `o` or a use of `o` as a `ref` or `out` argument ends the ref. A call of another method on `o` is not seen.
  - `ref r = xs[await Idx()]` is allowed, because the ref starts after its own statement. Another live ref gives the error about that ref.
  - A `.cpp` script keeps C++ semantics and has no `ref` lifetime rules.
- A view local of a named owner (`T[] s = v;`, `std.string_view w = this.label;`) is a checked path (B7). Each use takes the view of the owner again. It has these limits:
  - A view from a call (`var t = s.subspan(1)`), a view of a view parameter and a `var` view are plain views. A shape change of the owner can make them point to freed memory.
  - A view of a named owner cannot take a new value. Declare a new view.
  - With `HOT_RUNTIME_CHECKS` off, the index of a view is not checked.
  - A view that a lambda captures, a view parameter of an `async` method, and a view that a native function stores are not checked. The translator cannot tell a stored closure from an immediate call (a sort comparator). The owner can end first, and the view then points to freed memory. Copy the values (`std.vector<T>`, `std.string`) in such code.
  - A native function that takes a `ref`, a `T&` or a view can keep it. The checker cannot see this. The `as_ref` check only tests for null at the call.
  - A view in a container field of a native type or in a tuple is not checked. A `std.string_view` that a script call returns from a native getter has the lifetime that the native code gives it.
- A raw `T*` in a local or a parameter keeps its target address after a native free. A `CheckedPtr` field is checked, a local or a parameter is not. A native `T*` that the host frees while a script frame holds it is undefined behavior. This includes a frame that an `await` suspends. A reload does not free a native object. Keep a `CheckedPtr` or a handle, and check it again after a call that can free the object.
- A script class, struct or enum with the name of a C++ type from a `using` header. Rename the script type.
- Shared state in an `inline` function, an `inline` variable or a template static member of a host header. The script modules build with hidden symbol visibility. Each module gets its own copy of that state, and the host does not share it. Put such state in a `.cpp` file of the host, or mark it exported.
- A namespace block in a `.hot` script. Use `using ns;` for a native namespace.
- A call of a free function by name from the host through `Loader::thunk`. Call a method of a class.
- A function as a value, such as `var f = Add;`. Call it, or write a lambda.
- A `foreach` over a `std.map`, a `std.set` or an unordered container finds the current key again at each step and at each use of a `ref` loop variable. A `var` loop variable takes its copy without a second find. On a map of 4096 elements and an empty body, the loop is about 11 times slower than a plain C++ loop. A `foreach` over a vector, an array, a string or a span has no measured cost.
- A `foreach` over a span that a call returns, such as `foreach (var x in this.Make().View())`. The guard keeps the span, but not the object of the buffer. The span can point to freed memory after the call. A `foreach` over a member of a temporary, such as `this.Make().items`, is safe: the guard keeps the temporary alive.
- A `foreach` over a named `std.list`, `std.multimap` or another container with no index and no unique key: a compile error. Copy the elements to a `std.vector` first.
- A lambda that assigns a `foreach` variable, a structured binding, a `catch` variable, a parameter of an outer lambda or a `for` init local. Only a local or a plain parameter can be a shared cell (see doc/lambdas.md). Copy the value into a local first.
- An `async` free function or an `async` static method. An async function needs an object.
- A collection literal without a declared type: `var x = [1, 2];` is an error.
- A `.cpp` script has no null or bounds checks. The translator keeps a `.cpp` body as C++ text and does not parse expressions. The checks cover `.hot` scripts only.
- `front()`, `back()`, `top()`, `pop()`, `pop_back()` and `pop_front()` on an empty container throw `IndexOutOfRange`, also with `HOT_RUNTIME_CHECKS=0`. The check covers a call on a name or a member access. It does not cover `a?.back()`, `begin()`, `end()` or `*it`, nor a receiver of an unknown type (`var` of an unknown type). The receiver is the first argument of the helper, so it runs before the other arguments. `m.at(k)` with an integer key and a missing key throws `IndexOutOfRange`. A native exception becomes a `NativeError` only inside a `try` block that has a `catch` clause for `ScriptError` or `NativeError` and no clause for `...` or a native type. See [Runtime errors](statements-errors.html#runtime-errors).
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
- A C style cast to a type other than a builtin type or `T*`. Use `static_cast<T>(e)`. The error is `a C style cast is only for a builtin type or 'T*'`.
- Member access with `.` on an iterator or a `std.unique_ptr`. Write `(*p).x`. A `.` on a `std.optional` works: an empty optional throws `NullReference`.
- A constructor field initializer list (`A(int v) : x(v)`). Only `: base(...)` can follow the parameters. Set the field in the body: `this.x = v;`.
- A local function. Use a lambda (`var f = (int x) => x * 2;`) or a method.
- A switch expression (`v switch { 1 => a, _ => b }`) and patterns in `case` labels. Use a `switch` statement or `?:`. A type test is `is $C` or `as $C`.
- A tuple type `(A, B)` and a tuple expression `(a, b)`. Use `std.tuple<A, B>` and `std.make_tuple(a, b)`. `var (a, b) = e;` reads the parts.
- A default value on a `virtual` or `override` method. An override cannot change the default, so use an overload.
- A raw string literal (`R"(...)"`), the operator `<=>`, `goto` and `nameof(x)`. Each one is an error with its own message.
- A script reflection API and a name string of an enum value. An enum value has no members, so `c.ToString()` is a C++ error. Use a `switch` that returns the name. Custom attributes are for the host (see [Attributes](native-interop-attributes.html)).
- A `struct` field of a move-only type, such as `std.unique_ptr<T>`. A copy of a struct copies each field. Use a class: a class field of a move-only type moves across a reload (see [Move-only fields](hot-reload.html#move-only-fields)).
- Recursion deeper than 10000 script calls (1000 on Windows). The call throws `ScriptError("stack overflow")`, which a `catch` handles. The check is always on, also with `HOT_RUNTIME_CHECKS=0`. Every script member function, static method and free function counts one frame. Change the limit with `hot::set_max_call_depth(n)` (65519 at most). The macro `HOT_MAX_CALL_DEPTH` sets the default, but only when you compile the runtime library. The Windows default is lower because the main thread has a 1 MB stack. A script function frame uses about 100 to 400 bytes of stack in a Debug build, and more with many locals. A thread with a small stack (for example 512 KB on macOS) needs a lower limit.
- Integer `/`, `%`, `/=` and `%=` throw `ArithmeticError` for a zero divisor (see [Runtime errors](statements-errors.html#runtime-errors)). The check is always on, also with `HOT_RUNTIME_CHECKS=0`. The translator skips it when the divisor is a nonzero integer literal other than `-1`, or when an operand has a float type or a script class type. `INT_MIN / -1` wraps and does not trap.
- Integer `<<`, `>>`, `<<=` and `>>=` throw `ArithmeticError("shift count out of range")` for a negative count or a count not less than the bit width of the promoted left operand. The check is always on. The translator skips it when the count is a literal in range for a known left type, or when an operand has a float type or a script class type. A left shift wraps. A right shift of a negative value is arithmetic.
- Signed overflow wraps because the script compile has `-fwrapv`. The flag is in the JIT backend, the Clang files backend and the AOT library (`hot_add_scripts`). MSVC has no such flag: an MSVC build does not guard signed overflow in scripts. MSVC does not exploit it in practice.
- A null handle, a null `T*` or an expired `CheckedPtr` that a script passes to a native `T&` or `const T&` parameter (the plain form `F(w)`) throws `NullReference`. The check (`hot::as_ref`) is always on, also with `HOT_RUNTIME_CHECKS=0`. A null value passed to a native `T*` parameter stays `nullptr`, because null is a valid pointer argument. The member access `p.x` and `*p` on a null `T*` or a null handle keep the check of `HOT_RUNTIME_CHECKS`: with `HOT_RUNTIME_CHECKS=0` a null native pointer from a native call is undefined behavior. A `CheckedPtr` field is always checked. The cast `(T*)p` of a pointer or a handle (`hot::nonnull`) is always checked.
- A cast from a float type to an integer type gives the nearest limit for a value out of range, and `0` for `NaN` (`hot::float_to_int`). It covers `(T)f`, `T(f)` and `static_cast<T>(f)` when `T` is a builtin integer type or a C++ integer name. It does not cover: an implicit conversion (`int i = f;`, `i = f;`, `i += f;`, a float argument for an integer parameter, a `return` of a float), a cast to an alias of an integer type (`using Id = int;`), a cast to `bool`, and a cast that a native header writes. A `.cpp` script has no shift check and no float cast check, because the translator does not change it. It gets `-fwrapv`.
- The compound helpers `hot::div_assign`, `hot::mod_assign`, `hot::shl_assign` and `hot::shr_assign` take the target by reference. A bit-field target (`s.bf <<= 1`, `s.bf /= n`) does not compile. Write `s.bf = s.bf << 1`.
- A user `operator<<` or `operator>>` must be findable by argument-dependent lookup from the namespace `hot`, because `<<` becomes `hot::shl`. Put the operator in the namespace of its argument type.
- The frame count has gaps. A closure body, a coroutine body, and a free function or static method of a `.cpp` script do not count their frames, so a recursion only through them can still overflow the stack. A destructor does not count its own frame and cannot throw. While an exception unwinds the stack, a script function that a destructor calls can go 15 frames over the limit.
- More than 64 nested statements, 15 nested lambdas or 256 operators in one expression. Each level uses parser stack, so the limits are fixed. The parser reports `maximum nesting depth exceeded`.
- macOS. It is experimental: a CI job builds and tests it, and no maintainer runs it (see [macOS](macos.html)). Windows works with MSVC, and the CI tests it on Windows (see [Windows](windows.html)).
- A real Visual Studio or gdb debugger session. It is untested. An lldb session and an lldb-dap session run in the tests on Linux when lldb is installed.

A `.cpp` script (see [C++ scripts](cpp-scripts.html)) has more limits. It has no properties, no script virtual methods without `override`, no script-class handles and no `new`, and no script inheritance.

## What can still crash

A HotScript program does not crash the process through an invalid access, with these exceptions. Each item has its details in the list above. The table of the checks that exist is in [Checks against crashes](statements-errors.html#checks-against-crashes).

- A raw `T*` in a local or a parameter, after a native free. This includes a frame that an `await` suspends.
- A native function that stores a `ref`, a `T&` or a view. A view that a lambda or an `async` method keeps.
- A change of a container through an alias, another method or a free function, while a `ref` or a view uses it. A `foreach` has a run-time guard (`ContainerChanged`).
- A `.cpp` script. It has no null, bounds, shift, float cast or `ref` lifetime checks.
- A null native pointer from a native call, when you build with `HOT_RUNTIME_CHECKS=0`.
- A container with no bounds check, `a?.back()`, `begin()`, `end()` and `*it`.
- An implicit float to integer conversion that is out of range.
- Signed overflow in an MSVC build.
- A recursion only through a closure, a coroutine or a free function of a `.cpp` script. A thread with a small stack.
- A crash inside native code that the script calls.

## Limits

These limits come from the type system and the null analysis.

- A member-level `[nonnull]` is not available in a stub. A stub has no members. Use `[nonnull]` on a whole class.
- The analyzer does not check an argument to a `[nonnull]` parameter.
- `h.reset(rawptr)` is always an error. A shared pointer cannot come from a raw pointer.
- `$NativeBase b = this;` is an error, unless `enable_shared_from_this` is known for the native base.
- `T[]` is `hot::span<T>` and works with C++17. The option `HOT_VIEW_TYPE=std` selects `std::span<T>`, which needs C++20. The choice is one constant for the whole build. See [The view type](types.html#the-view-type).
- AOT code and modules: an importer includes the interface header of each import (`<name>.iface.h`), so a script parent in another unit links. In optimized AOT code (`OPTIMIZED`, `HOT_SCRIPT_MODE=AOT_OPTIMIZED`), an importer calls the exported forwarders of the owner unit (`hot_fn_<unit>__<symbol>`, `hot_mv_<unit>__<name>`). A unit of optimized code has no reload and no revert.
- A `.cpp` script that imports a `.hot` script: the classes of the import come after the text of the file. So a class of the `.cpp` script cannot have a script parent from the import.
- The lldb formatters (`tools/lldb/hot_formatters.py`) do not show fields or frames of optimized AOT code (`OPTIMIZED`, `HOT_SCRIPT_MODE=AOT_OPTIMIZED`).
