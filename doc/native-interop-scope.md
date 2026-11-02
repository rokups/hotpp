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
| `class Name;` | `Name` and all subclasses of `Name`, for `handle`, `weak_handle`, `intrusive` and `cpp_prelude`. `value` is for `Name` only. |
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

## Stubs in more than one prelude

The preludes are an ordered list. A library prelude sets the default rules, and a later prelude refines them. The script comes last. Two stubs have the same key when they name the same type in the same form: `class Name;`, `class : Name;`, `class *;`, `struct *;` or the same pattern.

A stub with the key of a stub in an earlier file merges into the earlier rule. It does not replace the full rule. The merge applies these rules:

| Attribute | Merge |
| --- | --- |
| `value`, `handle`, `intrusive` | The storage. A later stub that states one of them replaces the full earlier storage. For example, a new `handle` removes an inherited `intrusive`. |
| `weak_handle` | Replaces the earlier value. It belongs to the handle: a new storage without a handle removes it. A new handle with an inherited weak handle is an error, unless the later stub states `weak_handle` again. |
| `default_handle` | Replaces the earlier value. |
| `native`, `self`, `checked_index` | Add up. A later stub cannot remove them. |
| `cpp_prelude` | Adds up. The later text comes after the earlier text. |
| `cpp_overrides` | Adds up. The result has the names of all stubs, each name one time. |

An empty string removes an inherited attribute: `weak_handle("")`, `default_handle("")`, `cpp_prelude("")` and `cpp_overrides("")`. A new storage removes the old storage. The flags `native`, `self` and `checked_index` cannot be removed.

The translator checks the merged rule with the same checks as one stub. An error, such as `value` with an inherited `weak_handle`, shows at the later stub. In one file, a second stub with the same key is an error.

A later exact stub still needs `native`, as each exact stub does.

Example: a library prelude and a user prelude.

```hot
// lib.prelude.hot
[native, handle("$T*"), intrusive("AddRef", "Release"), cpp_prelude("HOT_OBJECT($N);")] class Object;

// user.prelude.hot
[native, cpp_prelude("HOT_TRACE($N);"), cpp_overrides("Update")] class Object;
```

The merged rule of `Object` keeps the intrusive handle. Each shell gets `HOT_OBJECT($N);` and then `HOT_TRACE($N);`.

## C++ insertions

The attribute `cpp_prelude("text")` puts C++ text at the start of a generated type. It is valid on a stub and on a class definition. A stub can be `class Name;`, `class : Base;`, `class *;` or `struct *;`.

- The text of a shell goes after the opening brace of the shell. The text of a script `struct` goes after the opening brace of the `struct`.
- `$T` is the generated C++ type that gets the text. `$B` is the direct C++ base of a shell. For a script `struct`, `$B` is empty.
- `$N` is the name of the script class, for example `Tool`. The translator writes the alias `using Tool = Tool_Shell;` before the shell, so `$N` is a C++ type name in the text.
- The string must be on one line. The translator does not change the text, and it writes each text on its own line with an indent of four spaces.
- A `cpp_prelude` on a class definition is for that class only.
- A `.cpp` script cannot write a `cpp_prelude`. Its shells get the texts of the stubs in the prelude files.

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

### Native virtuals that the host overrides

A host can override a native virtual in the text of a `cpp_prelude`. Then the attribute `cpp_overrides("names")` on the same stub gives the names of these virtuals. A space or a comma separates two names. The attribute is valid on `class Name;`, `class : Base;` and `class *;`. It is not valid on a class definition. It reaches the same classes as the `cpp_prelude` of the stub.

- The shell gets no slot and no stub for a virtual in the list. The text of the host is the only override in the shell.
- A script method that overrides a virtual in the list is an error.
- A pure virtual in the list does not need a script override.
- The text goes in each shell, also in the shell of a script child. There `$B` is the shell of the parent, so the text of the child can call the parent.

The shell keeps its address across a reload, and the loader keeps the code of the first module of a class. So a function-local `static` in the text stays the same object across a reload. A host can register its address. Two cases give a new object with the same value:

- A reload that changes the constructor list of a class. The new objects use the code of the new module.
- A new loader. It loads the modules again.

Example: a host type system. Each typed class reports a static type tag through a virtual.

```hot
using "TypedActor.h";

[cpp_prelude("HOT_TEST_TYPED($N, $B);"), cpp_overrides("GetTag")] class : TypedActor;

class Scout : TypedActor { }
class Ranger : Scout { }
```

The shell of `Ranger` starts with `HOT_TEST_TYPED(Ranger, Scout_Shell);`. The tag of `Ranger` names the tag of `Scout` as its base.
