# Handoff prompt: S++ hot-reload scripting prototype

You continue work on `/home/rk/src/spp`. This is a prototype of a hot-reloadable scripting language for C++. The script syntax is type-first, in the style of Vala and C#. The translator turns it into C++. Clang compiles the C++. The runtime reloads it and keeps live objects.

## Read first
1. `spp-design.md` in this project. This is the design. Read sections 4, 5, 7, 8 and 9. Use `grep -n '^## '` and `sed -n`.
2. `README.md` in this project. It describes the backends, the tools and the tests.

## Rules from the user
- Write all prose in ASD-STE100 Simplified Technical English. This includes code comments, documents, commit message bodies and subagent prompts. Maximum 20 words per sentence.
- All tests are GoogleTest. Register them with `gtest_discover_tests`.
- Fetch dependencies through CPM.cmake when they are not installed. Use `CPM_USE_LOCAL_PACKAGES ON`. Libclang and LLVM come from the system.
- Commit messages: short title, terse body. No attribution lines of any kind.
- Subagents: set `model` and `effort` on every Agent call. Use the cheapest suitable model. Use `sonnet` for implementation. Use `haiku` for lookups and test runs.
- Precompiling a script to a `.so` is not a backend. It is a tool (`sppc`). The project name is S++, the file extension is `.spp` and the C++ namespace is `spp`. The translator namespace is `sppc`.

## State: HEAD `1121b06` (Add task list and decision log)
All work is committed. The type-first syntax, properties, handle and value stubs, `ref`/`out` and the fixed-width builtin types are in `git log`. `TASKS.md` lists the slices S1 to S15 and their status. `DECISIONS.md` records the answers to the open design questions.

Layout of the sources:
- `src/translator/` is the translator (`sppc_lib`, `sppc`). `src/translator/parser/` has the lexy grammars. `src/runtime/` is the runtime. `src/tools/` has the tools.
- `include/` has the public headers. `example/` has `engine`, `scripts`, `imgui` and `bench_backends`.
- `tests/unit/translator/` has `parser`, `emit`, `lowering`, `translate`, `types`, `session` and `diagnostics`. `tests/unit/runtime/` has the runtime unit tests.
- `tests/integration/` has `backends`, `closures`, `reload` and `threading`. `tests/golden/` has the golden translations. `tests/support/` has the shared test headers.

| Part | What works |
|---|---|
| Engine and runtime | `minengine` with `Actor` and `World`. The script class is a real C++ subclass (shell) of the native base. State lives in a replaceable block. |
| Reload | Same object address. Migration by field name, `[alias(...)]` rename and type key. Validation without side effects. A rejected reload keeps the old generation. |
| Thunks, closures, pins | Delegates keep working after a reload. `spp::Closure` pins a generation. A stale closure logs one stale call and does not run. |
| Unload | A retired generation unloads when the pin count is zero and a frame passed. Generation 1 stays loaded while its class has live objects. |
| Threads | `World::TickParallel` with a barrier. The commit runs only at the barrier. ASan and TSan are clean. |
| Translator | `sppc_lib` works on strings. It reads the virtual methods of the native base from libclang. It emits `#line`. A session cache cuts a warm translate to about 1.3 ms. |
| Language subset | No brace initialization: use parentheses, `T(args)` and `new T(args)`. `using "Actor.h";` and `using <vector>;`, `class N : Base`, fields `Type name = init;`, `[alias("old")]`, methods `Ret name(T a) { }` with `void` and with the modifiers `override`, `virtual`, `new`, `public`, `private`, `protected`, properties, locals `Type x = e;`, `var x = e;`, `const Type X = e;`, `foreach (Type x in c)`, `throw`, `try`, `catch (Type e)`, `catch (...)`, `finally`, `this`, `super`, arrow lambdas `(T a) => ...`, `Math.Min` as `Math::Min`, native stubs `[native, handle("std::shared_ptr<$T>")] class Weapon;`, `[native, value] class std.string;` and pattern stubs (`std.vector<*>`, `physics.*`), `new T(args)`. Stubs live in the script or in a prelude file (`sppc --emit-cpp --prelude file`, `TranslateOptions::preludes`). Handle types use the storage type in a field, a local, a return and `foreach`; a handle parameter is `const H&`; `.` on a handle uses `spp::deref` (`spp::is_handle` for custom handles). A type with no stub keeps its raw text. `new T(args)` gives `std::make_shared<T>` (shared_ptr), `H(new T(args))` (other handle) or `T(args)`. A reload that changes a field type is rejected and the old generation stays. |
| Backends | `SPP_BACKEND=CLANG_FILES` (default; external clang++ through files) or `JIT` (in-process Clang plus ORC; no files). Chosen at build time. |
| Tools | `sppc` (default: precompile to `.so`; `--emit-cpp`: translate to C++), `bench_backends`. |
| Tests | 483 tests pass in `build` and `build-asan` (CLANG_FILES) after S5b. The JIT build adds a few tests (489 pass). Under load, `TranslateDiagnostics.TruncatedInputsTerminate` can exceed its time bound in `build-asan`; it passes alone. `build-tsan`: 330 of 332 pass. `StatementParseLimits.DeepNesting` overflows the stack and `TranslateDiagnostics.TruncatedInputsTerminate` exceeds its 5 s bound. Both come from the TSan overhead and are not data races. |

Build commands:
- `cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=clang++ && ninja -C build && ctest --test-dir build`
- Add `-DSPP_BACKEND=JIT` for the JIT backend. Add `-DCMAKE_CXX_FLAGS=-fsanitize=address` or `=thread` for sanitizers.

Measured on this machine (warm load of v2): JIT 808 ms, JIT with PCH 166 ms, CLANG_FILES 947 ms. Compile time dominates now.

## Not done (in this order of value)
1. Language: `void onReload(OldState old)` and the `convert` hook. Script inheritance and script virtual methods (`virtual` has no effect yet). String interpolation. `is` and `as`.
2. Generation 1 never unloads. Move the canonical `ClassRecord`, the factory, the thunks and the first vtable into a module that never unloads (design milestone M8).
3. Experiment F3: debugger check. lldb is not installed here (see `DECISIONS.md`); the DWARF line table has the script lines. Set a breakpoint in a script method and step. Check that lldb shows `player.spp` lines. The JIT backend registers no debugger plugin and uses `-gline-tables-only`.
4. Windows and macOS are not tested. The design lists the risks (RTTI comparison, symbol export, PDB).
5. The CPM fetch path never ran, because the system gtest was found. Test it with an environment where gtest is missing.
6. The translator cache hashes the content of a header that changed in the last 2 seconds. It also checks the paths that hide a header earlier in the include path. A header with an old time that an edit resets to the same time and size is not seen. `TranslatorSession::clear()` covers this.
7. Known workaround: libclang does not find its builtin headers here, so the build passes `-resource-dir` from `clang++ -print-resource-dir`.

## Syntax
The syntax is type-first. See `README.md` and `spp-design.md` (section 7) for the full list.

- `using "Actor.h";` and `using <vector>;` replace `import`.
- A class is `class Name : Base { ... }`.
- A field is `[alias("old")] [public|private|protected] Type name = init;`.
- A method is `[modifiers] Ret name(T a, ...) { }`. `void` means no return value. `override` is required when the method overrides a native virtual method.
- A constructor is `Name(T a, ...) : super(args) { body }`. The name is the name of the class. The `super(...)` call is optional. Without it, the base default constructor runs. The `super` arguments may use the constructor parameters only. A destructor is `~Name() { body }`. See `example/scripts/ctor_v1.spp`.
- Kinds of script class (S5a, design 7.10.1). The base is optional: `class Name { }` has no base.
  - A class with a native base is a handle. `new Name(args)` gives the handle of the base (stub rules), for example `std::shared_ptr<Name>(Name__new(args))`. A field, local or parameter of the class type uses the handle storage type.
  - A class without a base is a plain script class. It is also a handle (`std::shared_ptr` by default). Its shell derives from `spp::PlainObject` and `spp::ShellBase`. `super(...)` is an error in it.
  - `[value] class Stats { }` is a struct that lives inline in the state block. It has real copy semantics. `Stats(args)` and `new Stats(args)` both give the value. A reload migrates its fields recursively (`FieldInfo::sub`, `FieldAction::Nested`, design 5.3).
  - A `[value]` class has no base, no destructor, no `virtual` or `override`, no hook, no lambda and no `super`. It can use native types and other `[value]` classes only. The translator emits `[value]` classes first.
  - The order of the classes does not matter (S5b). The translator sorts them by use: a `[value]` class first, then each class after the classes that it uses. Two classes that use each other in a cycle are an error. A `[value]` class can use only native types and `[value]` classes. See `example/scripts/kinds_v1.spp` and `kinds_v2.spp`.
- A property is `Type N { get; set; }`, `{ get; }`, `{ get; private set; }` or `{ get { ... } set { ... } }`. The setter body reads the new value as `value`. See `example/scripts/props_v1.spp`.
- A read or a write of `p.N` on another object of the class calls `get_N` or `set_N` of that object. A write gives the assigned value, so `a = p.N = 1` works.
- A local is `int x = 5;`, `var x = 5;` or `const int X = 5;`. A loop is `foreach (Type x in c)`. A catch clause is `catch (Type e)` or `catch (...)`.
- An initializer is optional. No initializer means the default value (zero, `false`, null or the default constructor). `var` and `const` need one. A local without one prints as `T x = T();`.
- A parameter is `Type name`. A lambda is `(T a) => ...` or `(a) => ...`.
- Builtin types have a fixed width. Each alias maps to one canonical C++ name at parse time (`src/translator/builtin_types.h`): `bool`; `sbyte`/`int8` = `int8_t`; `byte`/`uint8` = `uint8_t`; `short`/`int16` = `int16_t`; `ushort`/`uint16` = `uint16_t`; `int`/`int32` = `int32_t`; `uint`/`uint32` = `uint32_t`; `long`/`int64` = `int64_t`; `ulong`/`uint64` = `uint64_t`; `nint`/`intptr` = `intptr_t`; `nuint`/`uintptr` = `uintptr_t`; `float`; `double`; `char` (8 bit); `wchar`/`char16` = `char16_t`. The canonical name is the emitted text, the override key and the reload type key. The parser rejects `unsigned`, `signed`, `long long`, `unsigned char` and the other C++ multi-word spellings with a hint such as "use 'ulong'". A native name such as `int32_t` stays a normal name. The override key also maps the libclang spelling of a native parameter (`int`, `unsigned`, `unsigned long long`) to the same fixed-width name (`key_norm` in `sppc_lib.cpp`). An alias in a template argument of an expression (`std.vector<ulong>()`) is canonical too. A bare name in an expression (`ulong(5)`) is not changed.
- These words are reserved: `var`, `foreach`, `in`, `using`, `override`, `virtual`, `new`, `public`, `private`, `protected`, `get`, `set`, `finally`, `ref`, `out` and `void`.
- The removed forms give a clear error: `function`, `let`, `import`, `name: Type`, `-> Ret`, `for ... of` and `@was(...)`.
- `.` is the only scope separator. The `[alias]` form did not change.

### Known limits
- The parser reads and stores the access words. The translator does not enforce them.
- The parser reads and stores `virtual` and `new`. They have no effect yet.
- The rewrite of `p.N` uses the name only, because the lowering has no types. It knows only the properties of the current class. If `p` is not an object of the class, the access throws `std::runtime_error`.
- Not in the parser: string interpolation, `is`, `as`. `new T(args)` of a native type works (parentheses only).
- Script constructors and `new` of a script class (S4):
  - How it works. The shell and the vtable stay in generation 1. So the canonical `ClassRecord` of generation 1 has a type-erased entry `construct`. It builds the shell from a tuple of the `super` arguments. A later generation calls it through `spp::construct_shell` (`include/spp/runtime/shell.h`) and then runs its own constructor body. The key `construct_key` is the type name of the tuple. A mismatch gives a `std::runtime_error`.
  - A constructor is not in the slot table. A reload does not run it again. A changed body applies to new objects only. The commit copies `ctor_default` and `dtor` to the canonical record. The loader does not touch `construct`.
  - A destructor runs from `destroy_state` through `ClassRecord::dtor`. The record of the current generation gives the code. A throw is logged and does not escape.
  - The engine factory `create()` runs the constructor with default values for the parameters (`ctor_default`). It uses the `super` arguments of generation 1.
  - Limits. One constructor and one destructor for each class. No overloads. No `ref`/`out` parameters in a constructor. No `&&` parameters.
  - `new Name(...)` works for the class itself and for the classes earlier in the file. The other order gives an error.
  - A class needs a constructor or one `new` use in generation 1. Else the canonical record has no `construct` entry and a later `new` throws "has no constructor entry". Restart the process in this case.
  - The types of the `super` arguments must not change after generation 1. A change gives the `construct_key` error at run time.
  - If a constructor body throws, the shell is deleted and the exception goes on. The script destructor still runs on the half-built object.
  - The result of `new` of a script class is a handle (S5a). The handle owns the object. An object that the engine makes (a factory) is still owned by the world.
  - Type-directed access (S5b). The lowering knows the script class of a local, a parameter, a field and a `new` result. Then `obj.field` reads the state of the object, `obj.Method(args)` calls the generation function directly, and `obj.P` calls the accessors of the class of `obj`. See `example/scripts/typed_v1.spp`.
  - Limits of the type-directed access: the type must be a script class of the same script. A local from `var x = native_call()` has no known type. A native type keeps the old rule: `obj.P` is a property when the current class has a property `P`. A script function used as a value (`var f = obj.M`) is an error. A `[value]` class member needs no help, because it is a real C++ member.
  - `return this` as a handle (S5b): a bare `this` where a handle type is expected (return, `var`, assignment, argument of a script function, property assignment) becomes `spp::handle_from_this<H>(self)`. A plain class gets `shared_from_this` from `spp::PlainObject`. A shell with a native base gets `std::enable_shared_from_this` only when the class uses `this` as a handle and the handle is `std::shared_ptr`. A reload that adds the first such use to a class changes the layout of the shell. Use it in generation 1. The object must be owned by a handle, else the call throws "'this' is not owned by a handle". A lambda does not convert `this`. The arguments of `new` are not converted. A null handle in `obj.field` throws `std::runtime_error`.
  - Parameter analysis (S5b, design 7.10.5). A value parameter of a `[value]` native type is `const T&` in the generation function, unless the body changes it, stores it or the type is small (trivially copyable, 16 bytes or less). A handle parameter is `const H&`, unless the body rebinds it or stores it (assignment, `return`, lambda, `new T(p)`). Then it is a copy. The slot adapter and the thunk keep the canonical form, so a body change does not change the call interface. Limits: the analysis is conservative. A member call `p.M()` counts as a change, because the pass does not know the constness of `M`. A `const T&` can alias a field that the body changes. The pass does not follow a parameter that goes to another function.
- Script inheritance (S6a, design 4.5, 7.6 and 7.7). See `example/scripts/inherit_v1.spp` and `inherit_v2.spp`. S6b: a family of three levels with a sibling is in `family_v1.spp` and `family_v2.spp`. `family_reparent.spp` changes the parent of a class: the loader rejects it.
  - Inherited properties (S6b). `this.P`, a bare `P` and `obj.P` find a property of the class or of a script parent. The nearest class wins. The access calls the accessor of the class that declares the property, for example `A__get_P(self)`. A child cannot override an accessor.
  - `var r = ref e` (S6b, design 7.10.6): a local reference is allowed. A lambda cannot capture it. A field, a return type or a collection element cannot hold a `ref` type. Storing `r` in a value field copies the value. The tests are in `tests/unit/translator/translate/borrowed_storage_translation_tests.cpp` (`RefLocal`).
  - `class Enemy : Player` with a script class `Player` of the same script. The shell of Enemy derives from the shell of Player. The state struct of Enemy derives from the state struct of Player. The field table of Enemy has the parent rows first, so a reload migrates both parts by name. `ClassRecord::super` points to the parent record. The root of the chain gives the native base.
  - A family (a root and all its children) has one slot numbering. The slots of the family come in the manifest order, so a slot number is stable over generations. The table of a class has holes (`nullptr`) for the functions of other branches. An inherited slot points to the function of the parent.
  - `virtual` adds a slot. A call of a virtual function goes through `Owner__f_virt(self, args)`. It reads the slot from the record of the object, so a call through a parent handle runs the child, also after a reload. `override` in a child fills the slot of the parent. It must have the same parameter types and the same return type. A function with the name of a parent function needs `new` (or `override` for a virtual). `super.f()` calls the parent implementation directly. `super.Tick()` of a native virtual calls `Parent__base_Tick`.
  - Constructors: `super(args)` of a child calls the constructor of the script parent first (`Child__ctor_chain`). The base arguments come from the chain of `super_args`. The destructor of the child runs first, then the destructor of the parent. A child without `onReload` uses the hook of the parent.
  - `obj is T` and `obj as T`. A script class `T` uses `spp::is_script` and `spp::as_script<T_Shell>`. They walk the `super` records by name. A native type uses `spp::is_native<T>` and `spp::as_native<T>` (`dynamic_cast`). `as` of a handle gives a handle that shares the owner. `as` of a raw pointer gives a raw shell pointer: use `var`. A `[value]` class is an error.
  - Limits. The parent must be in the same script. A reload that changes the script parent of a class is rejected. The `super(...)` arguments of a child are still evaluated two times: one time for the base arguments (`super_args`) and one time for the constructor of the parent (`ctor_chain`). S6b did not fix this. A fix needs one evaluation that feeds both the `construct` call and the chain, so `new`, `ctor_chain`, `ctor_default` and `create` must change together. Use `super(...)` arguments without side effects. A shell with children always gets `std::enable_shared_from_this` when its handle is `std::shared_ptr`. Multiple script inheritance is not supported.
- `ref e` and `out e` in a call argument give `spp::ref_arg(e)`. The prefix `&e` gives the error "use 'ref' instead of '&'". A `ref` of a handle converts to the raw pointer of its pointee, so a native `Actor*` parameter takes it (`include/spp/runtime/ref_arg.h`). `var r = ref this.pos` gives `auto& r = st->pos;`.
- Base stubs: a `handle` or `intrusive` stub on a native base class applies to all native subclasses. The nearest base wins. An exact stub on a subclass wins. A base stub beats a pattern stub. `value` is not inherited. The hierarchy comes from libclang. System-header classes, nested classes and templates are not in the map.
- `intrusive("AddRef","Release")` implies a handle. If `handle` is given, it must be `$T*`. It does not combine with `value`. The storage is `spp::Intrusive<T, &Owner::Add, &Owner::Release>`, with Owner the class with the stub. A new object starts with zero references. The wrapper adds a reference on construction from `T*`. Add and Release must be non-overloaded, non-template members. A missing method gives a clang error that names it. A reload copies the field (add) and destroys the old block (release), so the count stays correct.
- Deferred to slice 2 of "value and handle semantics" (S5b and later): the "store takes a copy" parameter analysis and conflict detection for multiple inheritance. A comparison of an `Intrusive` with a raw `T*` is untested.
- Translator checks (S2). `src/translator/check.cpp` walks the AST. `TypeModel::add_stubs` checks the stub rules. `translate_impl` asks libclang for the 7.10.7 facts.
  - `class *;` with `default_handle("H<$T>")` makes every native class without a stub a handle. Plain data (libclang: complete, trivially copyable, not polymorphic) stays a value. Order: exact stub, base handle, pattern stub, plain data, `class *`, raw. Two `class *` in one file, `value` on `*`, and two stubs for one type in one file are errors. A later prelude overrides an earlier one. A script stub that overrides a prelude stub gives a warning (`TranslateResult::warnings`, printed by `sppc` on stderr).
  - 7.10.7 checks use libclang constexpr probes (`sppc_facts`). The session caches the answers. A new question needs one more parse. `value` needs a copy constructor and a nothrow move constructor, and is an error on a non-final polymorphic type. `handle` needs `operator->`. `intrusive` needs both methods, not overloaded. A type that libclang does not find is skipped without an error.
  - The prelude is not in the cache key, because the cache holds header facts only and the translation is never cached. A test proves that a changed prelude changes the output.
  - Borrowed-storage rule (7.10.4, 7.10.6): a field, return value, collection element or lambda capture of a `T*`, `T&` or `ref` of a non-handle type. It is an error by default (S5b). `TranslateOptions::strict_borrowed = false` or `sppc --no-strict-borrowed` makes it a warning. The `player` example now keeps the name of the attacker in a `std.string` field. A stored `ref` follows the same switch.
  - Always an error (5.3): a script function used as a value (`var f = G;`, `Use(G)`).
  - Gaps: a `var` local from a native call has no known type, so a returned `T*` is not seen. A `ref x` passed to a native function that keeps it is not detectable. Pattern stubs (`std.vector<*>`) get no 7.10.7 check. A custom `spp::is_handle` is checked for `operator->` only. A pointer stored in a script field and kept across calls is seen only when the field type is written as `T*` or `T&`. `Weapon*` is allowed when `Weapon` is a handle.

## Parser

The translator parses the whole script. The grammar uses the lexy library. Only the files in `src/translator/parser/` include lexy headers.

- `script_grammar.cpp`: `using` lines, classes, fields, methods, properties, modifiers and `[alias]`. `parse_script()` is the entry point.
- `expression_grammar.cpp`: expressions. `parse_expression()` and `parse_expression_list()`.
- `statement_grammar.cpp`: statements, with `foreach`, `throw`, `try`, `catch` and `finally`. `parse_block()`.
- `lexy_common.hpp`: shared rules and error helpers. `span.h` has `Span` and `ParseInput`.
- `parser.h`, `expression.h` and `statement.h` are plain headers. They have no lexy include.

Design: span, then call. A grammar scans the raw text of a part and stores a `Span` (byte range). It does not parse the part. After the structure parse, `parse_script()` calls `parse_block()` on each method body and `parse_expression()` on each field initializer. The statement grammar calls `parse_expression()` on its spans. The expression grammar calls `parse_block()` for the body of a lambda. All positions are absolute byte offsets in the script text. `parse_script()` sorts its errors by position, so the errors come in source order.

After the parse, `lower_block()` and `lower_expr()` (`lower.cpp`) change script names to C++ names. The `Emitter` (`emit.cpp`) prints the AST with `#line` directives. `translate()` stops with `ok = false` when the parse gives an error.

How to add syntax:
1. Add the node type to `src/translator/ast.h` and to the `std::variant` of `Expr` or `Stmt`.
2. Add the rule in the right grammar file and a value callback that builds the node.
3. Add the printing in `emit.cpp` and, if the syntax changes names, the lowering in `lower.cpp`.
4. Add tests in `tests/unit/translator/`: `parser/` (round trip), `emit/`, `lowering/`. Run the golden tests (`tests/golden/`): they must stay byte-identical (a change of a builtin type name is the only reason to regenerate them).

Limits of the `.` access: the lowering writes `a.b` as `spp::deref(a).b`. A smart pointer, an iterator and `std::optional` have `operator->`, so `.` does not reach their members. Use `.get()` or `.operator*()`. A native global or a static data member (`Vec3.Zero.x`) becomes `Vec3::Zero.x`. Use a typed local for it. `spp::deref` returns an rvalue by value, so a member of a temporary keeps the temporary alive in a range-for. An array is deleted in `deref`.

Nesting limits: 64 nested statements, 15 nested lambdas (the method body is the first of 16 block levels), 256 operators in one expression. A deeper script gets an error.

Debug compile time of the grammar files: `script_grammar.cpp` about 3.3 s, `expression_grammar.cpp` about 5.6 s, `statement_grammar.cpp` about 4.5 s. Lexy templates are slow to compile. Keep each grammar in its own file and do not include the grammar headers in other files.

## How to work
- The main session coordinates. Give each step to one `implementer` agent (model `sonnet`) with a self-contained prompt: goal, files to read, rules above, verification commands, budget, stopping condition.
- Run implementers one at a time. They share files and the build directories.
- After each step, rebuild in a fresh directory and run `ctest` yourself before you accept the report.
- Use `engineer` (opus) only if an `implementer` fails twice at the same bounded task.
- Keep every claim in reports tied to a command output. Say what you did not run.

## Suggested next step
S5b is done except the items in TASKS.md. S6a (script inheritance, script virtuals, `is` and `as`) is done. Open from S5b: the type of a `var` from a native call, and the reload check for the `enable_shared_from_this` layout. Run the tests in `build`, `build-asan` and `build-jit`.

## Build time
- Compiler: clang++ 23.1.3, Debug (-g), lexy v2025.05.0, core target only.
- Object parser_probe.cpp: 465 ms (a TU without lexy headers: 197 ms). Clang frontend 440 ms, backend 29 ms.
- Target sppc_lib, clean build, Debug: 2254 ms (sppc_lib.cpp and parser_probe.cpp).
