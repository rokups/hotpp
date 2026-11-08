---
layout: default
title: Generics
nav_order: 10.5
group: Hot script
---

A struct, a class and a free function can have type parameters. The translator makes one plain copy of the declaration for each set of type arguments that the script uses.

```hot
struct Box<T> {
    T value;
    T Get() => value;
}

class Bag<T> {
    std.vector<T> items = std.vector<T>();
    void Add(T v) { items.push_back(v); }
}

T Larger<T>(T a, T b) => a > b ? a : b;

int Use() {
    Box<int> b;
    b.value = 3;
    var bag = new $Bag<$Child>();
    return Larger<int>(b.Get(), 2);
}
```

## Rules

- Write the type parameters after the name: `struct Box<T>`, `class Bag<T> : Base`, `T Larger<T>(T a, T b)`. A type parameter is a plain name.
- Write the type arguments at each use: `Box<int>`, `new $Bag<float>()`, `Larger<int>(1, 2)`, `Bag<float>.Kind()`. A call of a generic function needs the type arguments. The translator does not find them from the arguments.
- A type argument can be any type: `int`, `std.string`, `$Child`, `Child`, `std.vector<int>`, an alias, or another instantiation (`Box<Box<int>>`).
- In the body of a generic class or struct, the bare name is the current instantiation: `Box` in `Box<T>` is `Box<T>`.
- A type alias can name an instantiation: `using IntBox = Box<int>;`.
- A class can derive from an instantiation: `class Children : Bag<$Child> { }`.
- Each instantiation is a plain struct, class or function. All rules of plain declarations apply to it. The C++ name has the form `Box_G_int`.

## Checks

The translator checks the body of a generic once for each instantiation, with the type arguments in place. An error in the body has the position in the generic. The message names the instantiation as the script writes it: `Box<int>`. A generic with no use has a syntax check only.

There are no constraints (`where T : Shape`). A body can use any member of `T`. The check of each instantiation finds a wrong use.

## Units and imports

A generic is local to its unit. An importer cannot write `Box<float>` for a generic of an imported unit. The error is:

```
the generic 'Box' of the unit 'lib' is local to its unit. An importer cannot use it. Declare the generic in this script, or use a function of the unit
```

The instantiations that a unit uses are part of the unit. They are in the interface of the unit like plain declarations, so a change of their fields changes `iface_hash`. A function of the unit can return an instantiation to an importer: `var b = lib_make();`.

## Hot reload

An instantiation reloads like a plain declaration with its generated name. A change of a generic body changes all its instantiations. A new use adds a declaration. When the last use of a set of type arguments goes, the instantiation goes, like a removed class. A change of a type argument at a field (`Box<int>` to `Box<float>`) is a change of the field type.

## Not supported

Each item gives an error:

- A generic method in a class or a struct. Use a generic free function or a generic class.
- A generic interface and a generic enum.
- A type argument that the translator must find from the call arguments: `Larger(1, 2)`.
- A constraint: `struct Box<T> where T : Shape`, `T Pick<T : Shape>(T x)`. The check of each instantiation finds a wrong use of `T`.
- A generic with no type arguments: `Box b;`.
- A wrong count of type arguments.
- A type parameter that is not a plain name (`<int N>`), a duplicate type parameter, or a type parameter with the name of a builtin type.
- A generic `main`.
- A use of a generic of an imported unit.
- An instantiation in an instantiation deeper than 16 levels (a generic that uses itself with a larger type argument).
- A generated name that is also the name of a declaration of the module.

A `.cpp` script uses C++ templates.

## Design

Status: version 1 (2026-11-08).

- Instantiation model: the parser makes the instantiations (monomorphization at translate time). The scan in `src/translator/parser/generics.cpp` finds each generic declaration and blanks its parameter list, so each position stays. The parser then reads the declaration again for each set of type arguments. The type parameters are type aliases of the type arguments for that read (`ParseInput::aliases`). The later passes (checks, lowering, the emitter, the interface text, the backends) see only plain declarations. So JIT, files, AOT and optimized AOT code are the same.
- Uses: `apply_aliases` replaces each `Name<args>` in a type by the generated name, and `split_name` does the same in an expression. The list of instantiations grows while the parser reads the instantiations. The depth limit stops an endless chain.
- Names: `Name_G_arg1_G_arg2`. A character that is not a letter or a digit becomes `_`, and `$` becomes `H`. The name has no `__`, because the error rewrite splits generated names at `__`. Two different instantiations with the same name are an error, never a merge.
- Messages: the translator writes `Box<int>` for `Box_G_int` in its own messages. `TranslateResult::source_map` keeps the pairs (`SourceMap::generic_names()`), so `rewrite_diagnostic()` writes `Box<int>` in a C++ compiler message too. The traceback frames and the stale call text have the script name: `Box<int>.Get`. The registry keys keep the C++ name. A run-time message with the class name of a handle check (`hot::shell_of`) keeps the C++ name: the runtime uses that name to find the class.
- Decisions on the other forms (2026-11-08):
  - Type argument inference: not supported. The parser makes the instantiations, before the type check. The argument types are known only after the check (`types.cpp`). Inference needs a second parse and check of the script. The error tells the user to write `Name<...>(...)`.
  - Generic methods: not supported. The class body is read in one pass. A generic method needs a worklist for each class and slot keys for each instantiation of the method. A generic free function or a generic class does the same work.
  - Constraints: not supported. Each instantiation has a full check, so a wrong use of `T` gives an error at the instantiation. A constraint gives a clear error, and the scan blanks it, so the parse gives no second error.
  - Instantiation by an importer: not supported. Two importers would make two copies of one type in two modules, and `iface_hash` would not cover the generic body. A unit can export a function that uses an instantiation.
- Modules: the generic is not in the interface text. Instantiation by the importer is an error, so two units never have two copies of one type.
