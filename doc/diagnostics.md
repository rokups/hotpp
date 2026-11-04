---
layout: default
title: Diagnostics
nav_order: 20
group: Hot script
---

The tools show errors at the script line, with script names.

## Error format

An error has the form `file:line:col: error: message`. The next line is the script line. A note can follow. The last line is the count.

```
04_script_argument_missing.hot:11:41: error: no method or function 'Player.heal' fits these arguments
   11 |     override void Tick(float dt) { this.heal(); }
04_script_argument_missing.hot:7:10: note: candidate does not fit: takes 1 argument, but 0 were given
    7 |     void heal(float amount)  {
1 error generated.
```

## Common mistakes

Typos, wrong types and a missing `return` have plain messages.

```
01_typo_in_member.hot:11:49: error: script class 'Player' has no member 'Posiion'
   11 |     override void Tick(float dt) { var p = this.Posiion; }
1 error generated.

08_missing_return.hot:11:1: error: the method does not return a value on every path
   11 |     int Calc(int n) { if (n > 1) { return 1; } }
1 error generated.
```

A value that has no formatter in a `${...}` string, for example a native `enum class`, gives one error at the script line:

```
73_native_enum_in_interpolation.hot:6:16: error: 'Weather' cannot be printed in a '${...}' string: it has no formatter. For an enum class, cast the value to its integer type, for example 'static_cast<int>(value)'
    6 |         return "weather: ${current_weather()}";
```

## Null checks

The analyzer finds an access to a value that can be null. It runs on `.hot` scripts only. By default each finding is an error. The option `null_warnings` and the flag `hotc --null-warnings` make each finding a warning. Then the compile goes on.

```
p.hot:12:9: error: "p" can be null here; check it with "if (p != null)" or use "p?.m"
```

A field or an expression with no simple name uses the expression text. Sometimes the text is `this value can be null here; copy it to a local and check it`.

- A value of `T*`, `$T` or `T?` can be null.
- An access is `p.m`, a call, an index or a dereference.
- A value is not null in these cases: `this`, `new $T(...)`, `&x`, an `is $T name` binding, a box or a value local, a string literal, and a narrowed name.
- A native return is not null if its stub has `[nonnull]`. `[nonnull] class *;` sets a default for all stubs. A return is the `var` local type or a `this.f()` call.
- A comparison with `null` never gives a warning.
- An unknown type never gives a warning.

These forms narrow a name:

- `if (p == null) return;`. This also works with `break`, `continue` and `throw`.
- `if (p != null) { ... }` and the `else` branch of `if (p == null)`.
- `while (p != null)`.
- `p != null && p.m`, `p == null || p.m`, `p != null ? p.m : d` and `!`.
- `p is $T q`, `p?.m` and `p ?? d`.
- A cast to the same type, `(T*)p` or `($C)h`, throws on null. The result and the name `p` are not null after it. A downcast result can be null.
- The result of `p?.m` can be null: a handle member stays nullable, a value member gives `T?`.

Other rules:

- An assignment clears the narrowing. An assignment of a value that is not null sets it.
- A field never narrows. Copy the field to a local.
- A lambda body starts with no narrowing. A lambda that assigns a captured name removes its narrowing.
- A loop keeps the narrowing from the entry. It drops the narrowing of each name that the loop assigns.
- The narrowing from a `try` body does not hold in `catch`.
- `ref r = p;` to a nullable name disables the narrowing of both names.
- `ref r = this.f;` to a nullable field is always an error: `a ref cannot bind to a nullable field`. Copy the field to a local first.

Limits: a stub has no members, so `[nonnull]` works on a whole class only. A bare call to a native base method has an unknown type. A native `ref` or `out` argument does not clear the narrowing.

## Raw compiler text

`hotc --raw-diagnostics` prints the text of the C++ compiler as it is. Use it when a message is not clear. The option `raw_diagnostics` does the same in the backends.

## Debugging

The generated C++ has `#line` directives. A debugger shows `.hot` files and lines. A breakpoint on a script line works, and so does stepping.

- `gdb` and `lldb`: set a breakpoint with `breakpoint set --file player.hot --line 12` (`lldb`) or `break player.hot:12` (`gdb`).
- `lldb`: `tools/lldb/hot.lldbinit` loads formatters. The command `hot-bt` prints script frames.
- VS Code: the extension enables breakpoints in `.hot` files.
- Status: a real `gdb` or `lldb` session and Visual Studio are untested. Tests check only the `.hot` line table.
