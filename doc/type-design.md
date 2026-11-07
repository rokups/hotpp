---
layout: default
title: Type design
nav_order: 47
---

This page records an approved design for the types of HotScript. It is a design note. The translator does not implement all of it yet.

## Decisions

- Type operators are suffixes, except `$`, which is a prefix. `&` is not a type suffix.
- `const` is inferred. A script never writes it, except inside casts.
- A written `const` on a script local or a script method is an error.
- A `[native]` stub member keeps `const`.
- `struct` enforces POD. `class` has no such requirement.
- A rare exact mapping uses an attribute. It never uses a new keyword.
- Member access is always `.` in HotScript. The emitter writes `->` where C++ needs it.
- `extern` is a reserved word only. It has no syntax.
- Native declarations are `[native]` stubs and header imports.
- A suffix operator binds to the type. `int* a, b` declares two pointers.
- A prefix `$` also binds to the type. `$Foo a, b` declares two handles.
- An operator is a member method with the name `operator+`. The left operand is `this`. `!=` and the compound forms are generated. See [Operators](classes.html#operators).
- A `static` method and a free function have no object and no slot. They become plain C++ `static` functions. A reload replaces the module code. See [Static methods](classes.html#static-methods) and [Free functions](module-level.html#free-functions).

## Type mapping table

| HotScript | C++ | Rule |
|---|---|---|
| `T` (builtin, struct, native class) | `T` | A value. |
| `T` (script class) | `hot::Value<T>` | An owning box over the shell. See the notes below. |
| `T` (native class with a `handle` stub or a `default_handle`) | The handle form of the stub. | The stub sets the storage. `$T` names the same handle. |
| `T*` | `T*` | Borrowed. The analyzer treats it as nullable. |
| `const T*` | `const T*` | Only in casts. |
| `$T` | The handle form of the type rule. The default is `std::shared_ptr<T>`. | A shared owner. The analyzer treats it as nullable. A field with no initializer is null. |
| `T*?` and `$T?` | none | Not valid. `T*` and `$T` are already nullable. |
| `T?` | `std::optional<T>` | Value types only. It is an error on a pointer, a handle or a script class. |
| `T[]` | `hot::span<T>` (or `std::span<T>` with `HOT_VIEW_TYPE=std`) | A view. It is an error on a field and on a property. A local or a parameter is valid. A return of a local `std.vector<T>` or `T[N]` is an error, and so is a later use of a view after the source changed shape (see "Lifetime of a ref"). The index is checked. The owning list is the std prelude type `std.vector<T>`. |
| `T[N]` | `std::array<T, N>` | A fixed array. |
| `[weak] $T` | The weak template of the rule. The default is `std::weak_ptr<T>`. | A read emits `hot::lock(w)` and gives `$T`. |
| `int` | `int32_t` | |
| `long` | `int64_t` | |
| `float` | `float` | |
| `double` | `double` | |
| `bool` | `bool` | |
| `byte` | `uint8_t` | |

Notes on `hot::Value<T>`:

- It has one owner. It is never null. A copy clones the object.
- It is not inline, because the state layout changes for each generation.
- It is an error when the class has a native base. Write `$T` in that case.

## Parameter defaults

| HotScript | C++ |
|---|---|
| `T x` | `hot::In<T> x` |
| `in T x` | `const T&` |
| `ref T x` | `T&` |
| `out T x` | `T&`. The body assigns it on every path. |
| `[byval] T x` | `T` |
| `$T x` | `const std::shared_ptr<T>&` |
| `[byval] $T x` | `std::shared_ptr<T>` by value |
| `T* p` | `T*` |
| `T[] xs` | `hot::span<T>` by value (`std::span<T>` with `HOT_VIEW_TYPE=std`) |

- `hot::In<T>` is a C++ trait. It is `T` if `T` is trivially copyable and `sizeof(T) <= 16`. Else it is `const T&`.
- The translator never computes `sizeof`. A script `sizeof(T)` is the C++ `sizeof` (see statements.md).
- A plain parameter is read only. If the body writes it, or can write a field that it aliases, the body works on a by-value copy. The signature does not change.
- A `[byval]`, `in`, `ref` or `out` parameter keeps its own form. A `.cpp` script writes C++, so it has no `hot::In`.
- A constructor parameter that the body writes is a by-value parameter.
- A C++ error message shows `T` for `hot::In<T>`.
- An `override` or an interface method copies the base signature. The default rule applies to new methods only.
- A return type is by value, unless `ref` is written. A `ref T` return type gives `T&`. See "Lifetime of a ref".

## Locals and ref

- `var x = e;` gives `auto x = e;`. After const inference it gives `const auto x = e;`.
- `var x = null;` is an error.
- `ref r = x;` gives `auto& r = x;`. See "Lifetime of a ref".
- `foreach (ref x in c)` is the loop form of `ref`.
- `null` lowers by the target type:
  - `nullptr` for `T*`.
  - The empty handle for `$T`.
  - `std::nullopt` for `T?`.

### Lifetime of a ref

A `ref` names a place. A place has a root. The checker finds the root in the initializer. It removes parentheses, `.name`, `[index]` and a prefix `*`. The name that remains is the root.

| Root | Lifetime of the place |
|---|---|
| a local | the block of the local |
| a parameter (plain, `in`, `[byval]`) | the body |
| a `ref` or `out` parameter | the place of the caller |
| `this`, a field | the object |
| a module variable, a name that is not a local or a field | the program |
| a `ref` local | the lifetime of its own root |
| `c[i]`, `c.front()`, `c.back()`, `c.at(k)` | an element of `c`: valid until `c` changes its shape |
| `o.F(a, b)` where `F` returns `ref` | an element of any field of `o`, with the shortest of: the root of `o` and the root of each `ref` argument |
| `o.G(...)` where `o` is a native object or a handle to one | an element of `o`. A `[weak]` handle `o` gives a temporary |
| a literal, `new`, an operator result, a cast, `?.`, a string interpolation, a collection literal, any other call | a temporary: no lifetime |

Rules:

- R1 Bind. A `ref` local or `ref` loop variable needs a root with a lifetime. A temporary is an error.
- R2 Element. When the root is an element of `c`, `c` must not change shape while the `ref` lives. A `ref` lives to the end of its block. `foreach (ref x in c)` follows the same rule for the loop body.
  - A shape change is a call of one of these methods on `c`: push_back, emplace_back, pop_back, push_front, pop_front, emplace_front, emplace, emplace_hint, insert, insert_or_assign, try_emplace, erase, clear, resize, reserve, shrink_to_fit, assign, swap, rehash, merge, extract, append, replace, remove, remove_if or splice.
  - An assignment to `c` is a shape change.
  - Use of `c` as a `ref`, `out` or `&c` argument is a shape change. This includes a call such as `F(c)`, `this.F(c)` or `Class.F(c)` of an own method.
  - The owner of a place can be replaced: a handle `$T`, a pointer `T*`, a `T?` or a script class value. An assignment to the owner is a shape change for a `ref` that goes through it (`ref r = this.p.hp; this.p = new P();`). A struct value field is not replaceable, so `ref r = this.pos.x; this.pos = Vec3();` is allowed.
- R3 Escape. A `ref` cannot leave its block. It cannot go into a field, module variable, container element, `T?`, tuple, handle or lambda. It cannot cross an `await`. A type with `ref` inside is an error.
- R4 Return. A method with a `ref T` return type returns a place.
  - Each root of the place is `this`, a field, a module variable, a `ref` or `out` parameter, or a `ref` local with such roots.
  - A local, a plain/`in`/`[byval]` parameter and a temporary are errors.
  - `T` is a builtin, struct or native value type. A class type or a handle type is an error: return the handle `$T` by value.
  - An `async` method cannot return `ref`.
- R5 Call. The result of a `ref` call is a place (see the table). Rules R2 and R3 apply to each of its roots.
  - The result of `this.F(...)` can be an element of any field of the object. A shape change of any container, string, array or replaceable field of the object (`this.xs.push_back(2)`, `this.xs = other`, `xs.clear()`) ends the `ref`. A change of a scalar field does not.

Fix: copy the value (`var r = ...`), or keep the owner alive with a handle (`$T`).

Views. A view local acts like a `ref` to an element of its source. This holds for a local that has the declared type `T[]` or `std.string_view` and an initializer that is a local owner (`std.vector<T>`, `T[N]`, `std.string`) or a field of `this`.

- V1 Stale. A shape change of the source (the list in R2) makes the view stale. A later use of the stale view is an error. The assignment `s = x;` makes the view valid again. The view ends at the end of its block.
- V2 Return. A method that returns a view cannot return a local owner, a view local over a local owner, or a temporary vector. Rule R4 covers a `ref` return only.
- V3 Storage. A field cannot have the type `std.string_view` or `T[]`, also not inside a container type. A string literal as initializer is the exception.

A view differs from a `ref`: a view can cross a lambda or an `await`, and the checker does not follow it there (see doc/limits.md).

Note: the check sees direct mutation only. See doc/limits.md.

## new forms

| Script | C++ | Type |
|---|---|---|
| `new T(args)` on a struct or a native class | `T(args)` | `T` |
| `new T(args)` on a script class | `hot::Value<T>(T__new(args))` | `T` |
| `new $T(args)` on a script class, `std::shared_ptr` handle | `H(T__new_shared(args))` | `$T` |
| `new $T(args)` on a script class, other handle | `H(T__new(args))` | `$T` |
| `new $T(args)` on a native class | `std::make_shared<T>(args)` | `$T` |
| `new $S(args)` on a struct | `std::make_shared<S>(args)` | `$S` |

- A native class can use the factory of its stub rule. Example: the attribute `factory("std::make_shared<$T>")`.
- The `[value]` attribute is removed.

## $T rules

- A raw pointer cannot become a shared pointer. This applies to `this`, to any `T*` value and to any `.reset(p)` form.
- The error text is `cannot create a shared pointer from a raw pointer; use new $T(...)`.
- The exception is a `T` with a weak source. A weak source is `enable_shared_from_this` or `hot::weak_ref_traits`. Script shells have `hot::weak_ref_traits`.
- A script class with no base does not get `enable_shared_from_this`.
- `$Derived` converts to `$Base` where C++ allows it.
- `$T` converts to `std::weak_ptr<T>`. `.lock()` gives `$T`.
- `x.m` on a `T*` or a `$T` emits `x->m`.
- A native declaration writes `std::shared_ptr<T>` as `$T`.

## Const inference (v1)

- A local that is never assigned after its declaration emits `const`.
- A parameter of a non-virtual method emits `const` in the body, if the body never writes it.
- Methods and fields never change.
- A name that is the source of a `T[]` argument or of a `T[]` local is not `const`: the view can write it.
- A `virtual`, `override` or interface method takes its constness from the base.
- The pass runs after the checker and before emission.
- A written `const` on a local, a field or a method is the error `const is inferred; remove it`. A cast and a `[native]` stub can still name `const`.

## Nullability

- `T*` and `$T` can be null.
- A nullable value cannot be accessed before a null check. Access means `.m`, a call, an index or a dereference. An argument to a `[nonnull]` parameter is not checked yet.
- These values are known as non null:
  - `this`.
  - The result of `new $T(...)`. A `new T(...)` is a box value, which is never null.
  - A name bound by `is T name`.
  - A narrowed name.
  - A `[nonnull]` stub return.
- The narrowing forms are:
  - `if (p == null) return;`. This also works with `break`, `continue` and `throw`.
  - `if (p != null) {}`.
  - `p != null && p.m`.
  - `p == null || p.m`.
  - `p is T q`.
  - `p?.m`.
  - `p ?? d`.
- An assignment clears the narrowing.
- A field never narrows. Copy it to a local.
- A lambda body starts with no narrowing.
- A `ref` to a nullable name disables the narrowing of both names.
- A `ref` to a nullable field is an error.
- `?.` works on any expression. It gives a nullable result: a pointer or a handle member stays that type. A value member gives `T?`. The lowering is `hot::safe_access`.
- `x ?? y` gives `y` when `x` is null. When `y` is not null, the result is not null.
- `x is T name` binds a non null name.
- `x as T` gives a nullable result.
- `(T*)p` where `p` is `T*`, and `($C)h` where `h` is `$C` or a script `$Derived` of `C`, emit `hot::nonnull(p)`. It throws a recoverable `hot::NullReference`, also when `HOT_RUNTIME_CHECKS` is 0. The result is not null. The C++ compiler decides the type of `p` for `(T*)p`: the translator writes `hot::cast_ptr<T*>(p)`. `(T*)&x` stays a C cast.
- A downcast `($Derived)h` keeps `hot::as_script`. The result can be null.
- `(const T*)p` keeps the null state of `p`. `(T)x` for a `T?` is not a cast.
- The error text is `"p" can be null here; check it with "if (p != null)" or use "p?.m"`.
- A comparison with `null` is never a warning.
- Imports from libclang are nullable.
- A stub declares `[nonnull]` on a class. The returns of that class are not null. A stub has no members, so a member form is not possible. The members come from the C++ header. A member contract needs a header mark that libclang reads (see `doc/native-interop-attributes.md`). This is not done yet.
- `[nonnull] class *;` sets a library default.
- No script annotation exists.
- The analyzer uses the existing Diagnostics (add, warn). Each finding is an error by default. `--null-warnings` makes each finding a warning.
- Null access never crashes. It is an analyzer error at compile time, or `hot::NullReference` at run time.

## Native declarations

- `extern` is a reserved word. It has no syntax.
- A native declaration is a `[native]` stub or a header import.
- A stub member keeps `const`.
- A stub can declare `[nonnull]` and `factory("...")`.
- The "extern block" form goes away. Use native declarations.

## Keyword set

Reserved and used: `class`, `var`, `new`, `null`, `this`, `in`, `ref`, `out`, `virtual`, `override`, `extern`, `const` (casts and stubs), `return`, `if`, `else`, `while`, `for`, `foreach`, `break`, `continue`, `true`, `false`, `as`, `is`.

The design removes no project keyword. These keywords stay: `struct`, `enum`, `interface`, `using`, `async`, `await`, `abstract`, `public`, `private`, `protected`, `get`, `set`, `try`, `catch`, `finally`, `throw`, `switch`, `case`, `default`, `do`, `void`, `base`, `static` and the rest.

## Kept project mappings

- Primitives keep the project mapping: `int32_t`, `int64_t`, `uint8_t`, `bool`, `float`, `double`.
- These extra primitives stay: `sbyte`, `short`, `ushort`, `uint`, `ulong`, `nint`, `nuint`, `char`, `wchar`.
- `T[N]` gives `std::array<T, N>`. The module attribute `array` changes the template.
- The handle configuration stays: `handle("...")`, `default_handle("...")`, `intrusive("AddRef","Release")`, `[self]` and the weak template.
- `$T` on an intrusive stub can map to `T*`.
- `[weak]` stays an attribute.
- The `std.` prefix names the std types.
- A `.cpp` script keeps `hot::CheckedPtr` for `T*` fields.
- The C++ shape of a script class stays: `C_Shell`, `CState_gN` and the factories `C__new` and `C__new_shared`.
- `interface`, `enum`, `using`, `async`, `await` and properties stay.

## Migration table

| Old form | New form | Reason |
|---|---|---|
| `[value] T` | `T` | A bare script class is a value, so the attribute has no use. `[value] T` is an error. |
| `T& x` parameter | `ref T x`. Use `out T x` when the body only assigns it. | `&` is not a type suffix. The keyword shows the intent. |
| `const T& x` parameter | `in T x` | `in` is the read only form. `const` is inferred. |
| `T x` parameter that must stay a copy | `[byval] T x` | A plain parameter is read only and the translator picks the C++ form. |
| `ref var r = x;` | `ref r = x;` | `ref` replaces the pair of words. |
| `var& r = x;` and `foreach (var& x in c)` | `ref r = x;` and `foreach (ref x in c)` | Same reason: `&` is not a type suffix. |
| `Weapon w = null;` (a bare script class as a handle) | `$Weapon w;` | A bare script class is a value. `$` marks the shared owner. A value is never null. |
| `const` on a local, a field or a script method | Remove it. | The translator infers `const`. A cast and a `[native]` stub still name it. |
| `new T(args)` as a handle on a script class | `new $T(args)` | `new T(args)` gives a value. `new $T(args)` gives a handle. |
| Old owning `T[]` | `std.vector<T>` | `T[]` is now a view (`hot::span<T>`, or `std::span<T>` with `HOT_VIEW_TYPE=std`). The default works with C++17. |
| A checked pointer in script text | `T*`. It is borrowed and the analyzer treats it as nullable. | The null analysis checks each access at compile time. A `T*` field of a native class with a weak source is still `hot::CheckedPtr<T>` in the C++ output. |
| `--null-errors` | None. A null finding is an error by default. Use `--null-warnings` for warnings. | The default is the strict form. |
| An `extern` block | A `[native]` stub or a header import (`using "x.h";`) | `extern` is a reserved word only. It has no syntax. |
| `[native]` stubs | They stay. | A stub gives the handle rule and the attributes. |
