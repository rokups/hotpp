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

A change of the shape of a container while `foreach (var x in c)` walks it is undefined behavior in C++. The translator reports a compile error for such a change in the loop body. The shape changes are `c = ...`, and a call of `push_back`, `insert`, `erase`, `clear`, `resize`, `emplace`, `pop_back` and the other methods that can move or remove elements. Collect the changes in a new list, and apply them after the loop. A write to an element (`c[i] = v`, `x.field = v`) and a change of a different container are allowed. The check has limits. See [Limits](limits.html).
