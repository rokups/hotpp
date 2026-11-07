---
layout: default
title: try, catch and runtime errors
nav_order: 13
group: Hot script
---

## `try`, `catch`, `finally`

You throw and catch C++ exception types. A `finally` block always runs. A script can declare an exception type (see "Script exception types"). It can also catch the builtin types `NullReference`, `IndexOutOfRange`, `ArithmeticError`, `NativeError`, `ScriptError`, `TaskCancelled` and `TaskReloadCancelled` (see "Runtime errors").

```hot
class Safe {
    int Div(int x, int y) {
        try {
            if (y == 0) throw std.runtime_error("divide by zero");
            return x / y;
        } catch (std.exception e) {
            std.printf("error: %s\n", e.what());
            return 0;
        } catch (...) {
            throw;
        } finally {
            std.printf("done\n");
        }
    }
}
```

## Runtime errors

The runtime checks a pointer, a handle and an index of an array, a `std::vector` or any type with the stub attribute `checked_index` (see [Native interop](native-interop-checks.html#checks-on-a-stub)). A failed check throws one of these builtin types:

- `NullReference`: the script used a null pointer or a null handle, for example `p.x` or `*p`. A null handle, a null `T*` or an expired `CheckedPtr` passed to a native `T&` or `const T&` parameter also throws it. This check and the checked unwrap `(T*)p` are always on, also with `HOT_RUNTIME_CHECKS=0`. See [Limits](limits.html).
- `IndexOutOfRange`: an index is negative or not less than the size of an array, a `std::vector` or a checked native container.
- `ArithmeticError`: an integer `/`, `%`, `/=` or `%=` had a zero divisor. The message is `integer divide by zero` or `integer modulo by zero`. The check is always on, also with `HOT_RUNTIME_CHECKS=0`. `INT_MIN / -1` does not throw: it wraps to `INT_MIN`, and `INT_MIN % -1` is `0`. A float divide by zero gives `inf` or `nan` and does not throw.
- `ArithmeticError` also covers a shift: `<<`, `>>`, `<<=` or `>>=` with a negative count, or with a count not less than the bit width of the promoted left operand. The message is `shift count out of range`. The check is always on. A left shift wraps and does not throw. A right shift of a negative value copies the sign bit.
- `IndexOutOfRange` also covers `front()`, `back()`, `top()`, `pop()`, `pop_back()` and `pop_front()` on an empty container (vector, deque, list, string, queue, stack, `priority_queue` and each type with `empty()`). The message is `index 0 is out of range for an array of size 0`. The check is always on. `m.at(k)` with a missing key throws `ScriptError` with the message `key not found: ...`. `v.at(i)` with a bad index throws `IndexOutOfRange`.
- `NativeError`: a native exception that is not a `ScriptError` (`std::out_of_range`, `std::bad_optional_access`, `std::bad_alloc`, an exception of the host). A `try` block with a `catch (ScriptError e)` or `catch (NativeError e)` clause converts it. The message is `native exception <type>: <what>`. The conversion is off when the `try` has a clause for `...` or for a native type (`std.exception`, `std.out_of_range`). Then the clauses see the original exception, and a `ScriptError` clause does not catch a native exception. An exception that no script code handles goes to the script error handler with its demangled type name.
- `ScriptError`: the base of the types above. A call that is too deep throws a `ScriptError` with the message `stack overflow`. See [Limits](limits.html).
- `TaskCancelled` and `TaskReloadCancelled`: a task ends without a result. See [Async and await](async.html).

Write the type name in a `catch` clause. The prefix `hot.` is also valid. Use `catch (ScriptError e)` to catch all of them. `e.what()` gives the message.

```hot
class Grid {
    int[4] vals;
    Weapon weapon = null;

    int At(int i) {
        try {
            return this.vals[i];
        } catch (IndexOutOfRange e) {
            return -1;
        }
    }

    int Dmg() {
        try {
            return this.weapon.dmg;
        } catch (ScriptError e) {
            std.printf("error: %s\n", e.what());
            return 0;
        }
    }
}
```

A throw does not roll back. A field keeps each value that the script wrote before the throw. A script that does not catch the error passes it to the host. See [Error reports](embedding-rules.html#error-reports). The host can remove the checks at build time (`HOT_RUNTIME_CHECKS` in [CMake](cmake-options.html)). Then a null access or a bad index is undefined behavior.

## Script exception types

A `struct` with the base `ScriptError` is an exception type. The base can also be written `hot.ScriptError`, or it can be another exception struct. No new syntax is needed.

```hot
struct GameError : ScriptError {
    int code = 0;

    GameError(int c) : base("game error") { code = c; }
}

struct NetError : GameError {
    NetError(int c) : base(c) { }
}

class Player : Actor {
    int last = 0;

    void Run() {
        try {
            throw NetError(7);
        } catch (GameError e) {
            last = e.code;
            std.printf("%s\n", e.what());   // game error
        }
    }
}
```

- `throw E(args)` throws a value of the struct.
- `base("text")` in a constructor sets the message. `e.what()` gives it. A constructor that has no `base(...)` call uses the name of the first struct in the chain whose base is `ScriptError` as the message. A child struct without `base(...)` gets the name of its parent.
- `base(...)` of a struct with the base `ScriptError` takes exactly one message argument. A child struct passes its `base(...)` arguments to the constructor of the parent struct.
- `catch (E e)` catches `E` and each struct that derives from it. The variable is read-only.
- A script error that nothing catches goes to the host. The report shows the type `hot::ScriptError`, with the message of the exception.
- A script class (not a struct) is not an exception type. `throw new $C()` is an error: `cannot throw the script class 'C': declare 'struct C : ScriptError' for an exception`.

An exception struct is for `throw` and `catch` only. It cannot be the type of a field, a module variable or an element of a container such as `std.vector<E>`. A reload moves the state of a field with the Nested migration, and this migration does not keep the message of the base. A local variable and a parameter can have the type.

## Change of a container in a foreach

A `foreach` loop has a run-time guard. A change of the shape of the container in the loop body throws `ContainerChanged` (a `ScriptError`). The text is `container changed during foreach`. The guard works through any name, any method and any free function. A shape change is a change of the size, a new buffer, or a removed key. A write to an element (`c[i] = v`, `x.field = v`) is allowed.

```
foreach (var x in xs) { xs.push_back(x); }          // ContainerChanged at the next step
foreach (ref x in xs) { xs.push_back(1); x = 2; }   // ContainerChanged at "x = 2"
foreach (ref x in xs) { x = x + 1; }                // allowed
```

The guard has these forms:

- An indexed container (an array, `std.vector`, `std.string`, a span): the guard walks an index. Each step and each use of a `ref` variable compare the size and the address of the container object with the values at the loop start.
- A container with unique keys (`std.map`, `std.unordered_map`, `std.set`): the guard keeps a copy of the current key. Each step and each use of a `ref` variable find the key again. A removed key or a rehash throws.
- A temporary range (the result of a call): the body cannot change it. The guard walks it with a plain iterator.
- `range(a, b)` keeps the plain loop.

A range with a root that can move is evaluated again at each step: an element of a container (`this.rows[0]`), or a field behind a handle (`h.items`). A `ref` local, a view of a named owner and a `ref` loop variable of an outer `foreach` are paths too: the guard walks the path, not an old address. A member of a temporary (`this.Make().items`, `new $Q().items`) keeps the temporary alive in the guard during the loop. A named range of a different kind (`std.list`, `std.multimap`) is a compile error: copy the elements to a `std.vector` first. A `ref` variable of a `foreach` is not a C++ reference: each use reads the element through the guard.

## Checks against crashes

Each row shows how HotScript stops one cause of a crash. The mechanism is a compile error (the translator rejects the script), a runtime exception (a recoverable `ScriptError`), or unchecked. [What can still crash](limits.html#what-can-still-crash) lists the unchecked cases.

| Cause | Mechanism | Exception | Test |
|---|---|---|---|
| Null handle, pointer or optional | Compile error where the analysis sees null. Else runtime. | `NullReference` | golden `55`-`59`; `NullDerefCaughtInScript`; `StarNullCaught`; `RuntimeChecks.DerefOfNullPointerThrows`, `DerefOfAnEmptyOptionalThrows` |
| Null interface | Runtime | `ScriptError` | `CallOfANullInterfaceThrowsAScriptError`; `Fixture.NullHandleThrows` |
| Null or expired `CheckedPtr` | Runtime, always on | `NullReference` | `CheckedPtr.ArrowOnNullThrows`, `ArrowOnExpiredThrows`; `CheckedPtrFieldReadsNullAfterTheTargetDies` |
| Null handle to a native `T&` | Runtime, always on | `NullReference` | `NullHandleToNativeReferenceParameterIsCatchable`; `NullAlwaysOn.*` |
| Index out of range: vector, array, span, string | Runtime | `IndexOutOfRange` | `IndexOutOfRangeCaughtInScript`; `ViewParameterConvertsAndChecksIndex`; `RuntimeChecks.*IndexOutOfRangeThrows` |
| Missing key in a map | Runtime, `at` only | `IndexOutOfRange` or `NativeError` | `EmptyContainerOperationsAndNativeExceptionsAreCatchable`; `RuntimeChecks.IndexOfMapUsesItsOperator` |
| Empty `front`, `back`, `pop` | Runtime, always on | `IndexOutOfRange` | `EmptyContainerOperationsAndNativeExceptionsAreCatchable`; `container_ops_tests.cpp` |
| Dangling `ref` to a temporary, a local or a `ref` call result | Compile error | None | golden `27`, `28`, `82`-`88`, `94`-`100`, `108` |
| `ref` to an element or behind an owner after a shape change | Runtime: a checked path | `IndexOutOfRange` or `NullReference` | `RefCheckedPathsGiveANewElementOrAScriptError` |
| Dangling view: a return of a local owner, a view field | Compile error | None | golden `106`, `107` |
| View of a named owner after a shape change | Runtime: a checked path | `IndexOutOfRange` | `ViewOfANamedOwnerSeesTheNewBufferOrThrows` |
| Container change in `foreach` | Runtime, always on | `ContainerChanged` | `ForeachContainerChangeThrowsContainerChanged`; `foreach_tests.cpp` |
| Use after reload | A handle stays valid. A task of the old code is cancelled. | `TaskReloadCancelled` | `AsyncHost.ReloadCancelsTheSuspendedTask`; `TaskReload.*`; `CheckedPtrFieldKeepsItsTargetAcrossReload` |
| Use after a native free | `CheckedPtr` and handles are checked. A raw `T*` is unchecked. | `NullReference` | `CheckedPtrFieldReadsNullAfterTheTargetDies` |
| Integer divide or modulo by zero | Runtime, always on | `ArithmeticError` | `IntegerDivideAndModuloAreChecked`; `arith_tests.cpp` |
| Shift out of range | Runtime, always on | `ArithmeticError` | `ShiftsOverflowAndFloatCastsAreDefined`; `arith_tests.cpp` |
| Signed overflow, float to int cast | Defined result (`-fwrapv`, saturation) | None | `SignedOverflowWrapsWithTheOptimizer`; `ShiftsOverflowAndFloatCastsAreDefined` |
| Deep recursion | Runtime, always on | `ScriptError("stack overflow")` | `InfiniteRecursionIsCaughtInScript`; `RecursionLimitIsExactAndCanChange` |
| Bad `as` or derived cast | Gives a null handle. The use is checked as null. | `NullReference` | `BadAsAndDerivedCastGiveNullAndTheUseThrows`; `IsAs.ScriptClassesWalkTheParentRecords`; `CheckedUnwrapCastThrowsOnNull` |
| `await` on a dead task | Runtime | `ScriptError` (`TaskCancelled` for a cancel) | `TaskTests.EmptyTaskAwaitThrows`, `DestroyOfAnAwaitedChildGivesTheParentAnError`; `TaskReload.AwaitOfATaskCreatedBeforeAReloadGetsTheError` |
