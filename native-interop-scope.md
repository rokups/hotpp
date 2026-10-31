---
layout: default
title: Scope of a stub and C++ insertions
nav_order: 17
group: Hot script
---

## Scope of a stub

A stub name decides which types the rule covers.

| Stub | Types that get the rule |
| --- | --- |
| `class Name;` | `Name` and all subclasses of `Name`, for `handle`, `intrusive` and `cpp_prelude`. `value` is for `Name` only. |
| `[self] class Name;` | `Name` only. No subclass gets the rule. |
| `class : Base;` | The subclasses of `Base` only. `Base` does not get the rule. |
| `class *;` | Every class that has no more specific rule. |
| `struct *;` | Every script `struct`. Only `cpp_prelude` is valid. |
| `class ns.*;` and `class T<*>;` | Every type whose name starts with the prefix. |

The attribute `checked_index` is an exception: it is valid on an exact name and on `T<*>` only, and it does not reach subclasses (see [Bounds check on an index](native-interop-checks.html#bounds-check-on-an-index)).

`self` is valid only on an exact name. It is not valid on `*`, on a pattern or on `class : Base;`. `class : Base;` accepts `native` but does not need it. It does not accept `value` or `default_handle`. The attribute `native` is optional on `class *;` and on `class : Base;`. A rule `class : Base;` that has only `cpp_prelude` and no `native` does not change the storage of a type. A file can have only one `class : Base;` for each `Base`, one `class *;` and one `struct *;`.

The translator picks the rule in this order:

1. A script class.
2. An exact stub of the type.
3. The bases of the type, nearest base first. For each base, the rule `class : Base;` wins. Else the exact stub of the base wins if it has a handle and no `self`.
4. The longest pattern.
5. A plain data class is a value.
6. `class *;`.
7. A raw type.

Example 1: only the subclasses of `Object` get the shared handle.

```hot
using "Object.h";

[native, handle("std::shared_ptr<$T>")] class : Object;

class Cache {
    Object root;       // Object itself: not a handle
    Item item = null;  // a subclass of Object: std::shared_ptr<Item>
}
```

Example 2: `self` keeps the handle on `Object` only.

```hot
using "Object.h";

[native, handle("$T*"), intrusive("AddRef", "Release"), self] class Object;

class Cache {
    Object root = null;  // intrusive handle
    Item item;           // a subclass: no handle
}
```

Example 3: a rule for every class, and an exception for one type. The exact stub of `Item` wins over the stub of `Object`.

```hot
using "Object.h";
using "Actor.h";

[native, handle("$T*"), intrusive("AddRef", "Release")] class Object;
[native, default_handle("std::shared_ptr<$T>")] class *;
[native] class Item;

class Cache {
    Object o = null;   // the stub of Object
    Item item;         // the stub of Item: raw
    Actor a = null;    // no stub: the fallback handle
}
```

## C++ insertions

The attribute `cpp_prelude("text")` puts C++ text at the start of a generated type. It is valid on a stub and on a class definition. A stub can be `class Name;`, `class : Base;`, `class *;` or `struct *;`.

- The text of a shell goes after the opening brace of the shell. The text of a script `struct` goes after the opening brace of the `struct`.
- `$T` is the generated C++ type that gets the text. `$B` is the direct C++ base of a shell. For a script `struct`, `$B` is empty.
- The string must be on one line. The translator does not change the text, and it writes each text on its own line with an indent of four spaces.
- A `cpp_prelude` on a class definition is for that class only.
- A `.cpp` script has no `cpp_prelude`.

A type gets the texts in this order:

1. The text of `class *;` (a shell) or of `struct *;` (a `struct`).
2. The texts of the bases, from the farthest base to the nearest. For each base, the text of the exact stub comes first (if it has no `self`), then the text of `class : Base;`.
3. The text of the class definition.

```hot
using "Actor.h";
using "Object.h";

[native, handle("std::shared_ptr<$T>")] class : Object;
[cpp_prelude("OBJECT_DATA;")] class *;

[cpp_prelude("HOT_OBJECT($T, $B);")]
class Tool : Object {
    int uses;
}
```

The shell of `Tool` starts with these lines:

```cpp
    OBJECT_DATA;
    HOT_OBJECT(Tool_Shell, Object);
```

A `cpp_prelude` on a native type with `self` has no effect, because the type has no shell. The translator gives a warning.
