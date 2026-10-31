---
layout: default
title: try, catch and runtime errors
nav_order: 13
group: Hot script
---

## `try`, `catch`, `finally`

You throw and catch C++ exception types. A `finally` block always runs. A script cannot declare an exception type. It can catch the builtin types `NullReference`, `IndexOutOfRange` and `ScriptError` (see "Runtime errors").

```hot
class Safe {
    int Div(int x, int y) {
        try {
            if (y == 0) throw std.runtime_error("divide by zero");
            return x / y;
        } catch (const std.exception& e) {
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

- `NullReference`: the script used a null pointer or a null handle, for example `p.x` or `*p`.
- `IndexOutOfRange`: an index is negative or not less than the size of an array, a `std::vector` or a checked native container.
- `ScriptError`: the base of the two types above.

Write the type name in a `catch` clause. The prefix `hot.` is also valid. Use `catch (ScriptError e)` to catch both types. `e.what()` gives the message.

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
