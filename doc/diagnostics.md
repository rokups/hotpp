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

## Lint warnings

The lint pass runs on `.hot` scripts only. Each finding is a warning. The compile goes on. A warning has the script file, the line and the column.

```
p.hot:12:13: warning: local variable 'x' is never used. Remove it, or start its name with '_'
p.hot:15:9: warning: unreachable code: this statement follows 'return' and never runs
p.hot:20:17: warning: 'i' hides the local variable 'i' that is declared at line 18
```

**Dead code.** A statement that follows `return`, `break`, `continue` or `throw` in the same block never runs. The pass warns once for the first such statement of the block. It does not look into `if` branches: a `return` in one branch is not an exit of the block. In a `switch` case, a lone `break;` after the exit of the case gives no warning. This holds only when the `break;` is a direct statement of the case. In `case 1: { return; break; }` the `break;` is in a block of its own, so it warns.

**Unused local.** A local that the body never reads gives `local variable 'x' is never used`. The warning also covers a local with a side effect in the initializer, for example `int x = this.calc();`. The pass cannot know if the effect is wanted. To keep the call and drop the warning, start the name with `_`. The rules:

- A name that starts with `_` is exempt.
- A read in a nested block, in a lambda, in a `${...}` string or as a call argument (also for an `out` parameter) is a use.
- `x += 1`, `x++` and a member write such as `x.field = 1` are uses.
- The statement `x = e;` is a write. A local that has only writes is unused. A later read makes it used.
- A parameter, a `foreach` variable, a `catch` variable, a `var (a, b)` binding and an `is T name` binding never give this warning.
- A body that has C++ text the pass cannot read gives no unused warning.
- A local that holds an object only for its constructor and destructor (RAII), for example `$Wrap w = new $Wrap(o);`, also warns, because the script never reads it. Start the name with `_`, for example `_w`.

**Shadowing.** A local, a lambda parameter, a `foreach` variable, a `catch` variable, a name of a `var (a, b)` or `foreach (var (k, v) in c)` binding, or an `is T name` binding that has the name of a local, a binding or a parameter of the enclosing function (also an outer function of a lambda) gives `'x' hides ...`. The message says if the hidden name is a parameter, a local or a pattern binding, and the line of the local or the binding. The scope of an `is T name` binding is the condition and the `then` branch. So `if (o is A n) { } else if (o is B n) { }` gives no warning. The name `_` is exempt. A field does not count: a local with the name of a field is common, and `this.name` still reaches the field. Two sibling blocks, two sibling lambdas or two `if` statements can use the same name.

The pass is in `src/translator/lint.cpp`. There is no option to turn it off.

## Raw compiler text

`hotc --raw-diagnostics` prints the text of the C++ compiler as it is. Use it when a message is not clear. The option `raw_diagnostics` does the same in the backends.

## Debugging

The generated C++ has `#line` directives. A debugger shows `.hot` files and lines. A breakpoint on a script line works, and so does stepping.

- `gdb` and `lldb`: set a breakpoint with `breakpoint set --file player.hot --line 12` (`lldb`) or `break player.hot:12` (`gdb`).
- `lldb`: `tools/lldb/hot.lldbinit` loads formatters. The command `hot-bt` prints script frames.
- VS Code: the extension enables breakpoints in `.hot` files.
- Status: a real `gdb` or `lldb` session and Visual Studio are untested. Tests check only the `.hot` line table.
