---
layout: default
title: Native interop
nav_order: 11
group: Hot script
---

A script calls engine code directly. There is no wrapper code.

## Engine functions and C++ types

Include the header with `using`. Then call its functions, methods and types. Use `.` for every scope. Use `T(args)` to build a value.

```hot
using <algorithm>;
using "Actor.h";
using "Math.h";
using "Util.h";

class Tracker : Actor {
    std.vector<int> history = std.vector<int>();
    std.string label = "tracker";

    override void Tick(float dt) {
        var v = std.vector<float>();                        // template from C++
        v.push_back(4.0);
        v.push_back(2.5);
        std.sort(v.begin(), v.end(), std.greater<float>());
        float low = Math.Min(v[0], v[1]);                   // static function
        float d = Distance(Position, Vec3(0, 0, 0));   // free function
        Name = FormatVec(Position);               // returns std.string
        history.push_back(static_cast<int>(history.size()));
    }
}
```

## Native objects as parameters

Take a native object as `T&`. You call its methods and read its members. The overload comes from the argument types.

```hot
using "Actor.h";

class Hitter : Actor {
    void Hit(Actor& other) {
        other.TakeDamage(5.0);
        other.TakeDamage(*this, 2.0);
        other.Move(Vec3(1, 0, 0));
        var name = other.Name;          // a copy of the member
    }
}
```

A script handle `Weapon w` goes to a native `Use(Weapon*)` or `Use(const Weapon&)` in the plain form: `Use(w)`. The translator writes `hot::as_ptr(w)` for `T*` and `const T*`. It writes `hot::as_ref(w)` for `T&` and `const T&`. A null handle gives `nullptr` for a pointer. A null handle throws `NullReference` for a reference. A parameter of the handle type (`std::shared_ptr<Weapon>`) takes `w` as it is.

The conversion needs a clear parameter form. All overloads of the function with this number of arguments must take the argument in the same form. When the overloads disagree, for example `Mixed(Weapon*)` and `Mixed(const Weapon&)`, nothing converts. Write `Mixed(&*w)` or `Mixed(*w)`. The conversion applies to an argument that is a variable, a field or a member chain. Other expressions keep their form. A `.cpp` script does not convert.

A pointer from a temporary handle is valid to the end of the full expression. Do not store it. A handle in a local or a field keeps the object alive.

## Native API patterns

The translator reads the native signature. You write the call in the plain form.

```cpp
// Armory.h (engine)
struct Armory {
    bool Damage(int id, int& dmg);           // out-parameter
    Weapon* Best();                          // can return nullptr
    bool Find(int id, Weapon** out);         // out-pointer
    std::optional<int> Pick(int id);
    std::shared_ptr<Vec3> Origin();
    void Hit(const Weapon& w);
    void Use(Weapon* w);
    std::vector<int> ids;
};
```

```hot
using "Armory.h";

[native, handle("std::shared_ptr<$T>")] class Weapon;

class Shop : Actor {
    int total = 0;

    void Run(Armory& a, Weapon h) {
        int dmg = 0;
        if (a.Damage(7, dmg)) total += dmg;       // T& out-parameter: no marker

        Weapon* best = a.Best();                       // T* result: compare with null
        if (best != null) total += best.dmg;

        Weapon* w = null;
        if (a.Find(7, &w)) total += w.dmg;        // Weapon** takes &w

        var it = a.ids.begin();
        total += *it;                             // iterator

        var o = a.Pick(3);
        if (o.has_value()) total += *o;           // std.optional

        var p = a.Origin();
        float x = (*p).x;                              // smart pointer

        a.Hit(*h);                                     // handle to const T&
        a.Use(&*h);                                    // handle to T*
    }
}
```

`&h` on a handle is an error. Pass `h` for a handle parameter, or for a native `T*` or `T&` parameter. `&*h` for a `T*` and `*h` for a `T&` stay valid. `.` on a handle always means a member of the pointee, so `h.get()` calls `get` of the pointee. Inside a method, `this` is a handle for a handle parameter. For a native `Actor&` parameter, write `*this`. A `T&` parameter of a script class takes a variable, not `this`: write `Hero me = this;` first.

## Fallback handle

`[native, default_handle("std::shared_ptr<$T>")] class *;` sets the handle for every native class that has no stub of its own.

```hot
using "Weapon.h";

[native, default_handle("std::shared_ptr<$T>")] class *;

class Smith {
    Weapon w = null;       // Weapon has no stub: it gets the fallback handle
}
```

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

## Types with operator->

`.` works on a value, a pointer, a reference and a `std.shared_ptr`. It does not work on an iterator, a `std.optional` or a `std.unique_ptr`. Write `(*p).x` for these types.

```hot
using "Actor.h";

class Holder : Actor {
    void Run(std.unique_ptr<Actor>& p) {
        // p.Position.x does not compile
        float x = (*p).Position.x;
    }
}
```

## Checks on a stub

The translator checks a stub against the native type.

- `value` needs a copy constructor and a move constructor that does not throw. A reload moves the state block.
- `value` on a polymorphic type that is not `final` is an error. This prevents slicing.
- `intrusive` needs the two named methods, and they must not be overloaded.

## Module attributes

A module attribute list is an attribute list that ends with `;` and has no item.

```
[std("eastl")];
```

The attributes apply to the module (the file) that holds the list. A bare `;` is valid and does nothing.

The attribute `std("name")` sets the name of the standard namespace. The default is `std`. A host that uses another library, such as EASTL, sets it one time in a prelude. The generated C++ then writes `eastl::string`, `eastl::vector`, `eastl::shared_ptr` and so on.

- A list in a prelude is the default for each module of the host.
- A list in a script overrides the default for that script only.
- One attribute name is valid one time in one file. A repeat is an error.
- Two preludes cannot set the same attribute.
- An imported script uses the namespace of the main script. It can set only that same namespace.

In a script, `std.x` is the name `x` in the configured namespace. A script that writes `std.string` gets `eastl::string`.

With a namespace other than `std`, the script cannot name the real `std`. The name `std.x` always means `x` in the configured namespace.

An interpolated string does not follow the configured namespace. It uses `std::format` or `fmt::format` (the `--format` option) and gives a `std::string`.

A string in a stub can use `$std::` for the configured namespace:

```
[std("eastl")];
[native, default_handle("$std::shared_ptr<$T>")] class *;
```

Plain `std::` in a stub string stays as you write it. The names of `<type_traits>`, `size_t` and the C functions always use `std::`.

The host must declare the namespace. For a test, `namespace mystd = std;` is enough. Use the same namespace in the C++ text that libclang reads.

### The array type

The attribute `array("template")` sets the C++ type of a fixed-size array `T[N]` of a script. The default is `$std::array<$T, $N>`.

```
[array("my::Fixed<$T, $N>")];
```

- `$T` is the element type. `$N` is the size.
- The template must hold both `$T` and `$N`. Another `$` name is an error.
- `$std::` is the configured standard namespace. With `[std("eastl")]`, the default is `eastl::array<T, N>`.
- A multi-dimension array nests the template. `int[2][3]` becomes `my::Fixed<my::Fixed<int32_t, 3>, 2>`.
- The scope rules are the same as for `std`. A list in a prelude is the default of the host. A list in a script overrides it. A repeat in one file is an error. An imported script uses the value of the main script and cannot set another value.
- Both attributes can be in one list: `[std("eastl"), array("$std::array<$T, $N>")];`.

The script writes an index as `a[i]`. The generated C++ calls `hot::index`. `hot::index` checks the bounds of `std::array` and of a C array. A custom array type uses its own `operator[]` and has no check from the runtime.

A custom array type has no bounds check. The host should also specialize `hot::holds_no_code` for the custom array type, as the runtime does for `std::array`. Without it, a custom array type that is not trivially copyable counts as a type that can hold code.

A stub in a prelude can name an array type only in the base of a stub such as `class : Wrap<int[3]>;`. That type uses the template of the earlier preludes and of the same prelude.

## Other limits

- Write `.`, not `::` or `->`.
- `T(args)` for a native aggregate needs C++20. In a C++17 build, the native type needs a constructor.
- A native array has no `.` access.
- A native static data member, such as `Vec3.Zero.x`, needs a typed local first.
- A function that the script calls must be exported by the host. Link with `ENABLE_EXPORTS`.
