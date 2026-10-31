---
layout: default
title: Diagnostics
nav_order: 12
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

## Raw compiler text

`hotc --raw-diagnostics` prints the text of the C++ compiler as it is. Use it when a message is not clear. The option `raw_diagnostics` does the same in the backends.

## Debugging

The generated C++ has `#line` directives. A debugger shows `.hot` files and lines. A breakpoint on a script line works, and so does stepping.

- gdb and lldb: set a breakpoint with `breakpoint set --file player.hot --line 12` (lldb) or `break player.hot:12` (gdb).
- lldb: `tools/lldb/hot.lldbinit` loads formatters. The command `hot-bt` prints script frames.
- VS Code: the extension enables breakpoints in `.hot` files.
- Status: a real gdb or lldb session and Visual Studio are untested. Tests check only the `.hot` line table.
