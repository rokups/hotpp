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

The attribute `checked_index` is an exception: it is valid on an exact name and on `T<*>` only, and it does not reach subclasses (see "Bounds check on an index").

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

## Bounds check on an index

The script writes an index as `a[i]`. The generated C++ calls `hot::index(a, i)`. A type that is *checked* gets a bounds test. An index that is negative or not less than `size()` throws `IndexOutOfRange`. Other types use their own `operator[]`.

These types are checked by default:

- A C array.
- `std::array<T, N>`.
- `std::vector<T, A>`. This is a change: an index outside a vector was undefined behavior before. Now it throws.
- The array type `T[N]` of the script. This includes a custom type from the module attribute `array` (see "The array type").

Other types are not checked. For example `std::map` keeps its own `operator[]`.

The stub attribute `checked_index` has no argument. It makes a native type checked:

```hot
using "Ring.h";

[native, checked_index] class my.Ring<*>;   // every my::Ring<T>
[native, checked_index] class my.Buffer;    // my::Buffer only

class Wave {
    my.Ring<float> samples;
    float Last() { return this.samples[this.samples.size() - 1]; }
}
```

- The type needs `size()` and an `operator[]` that takes `std::size_t`.
- `checked_index` is valid on an exact name and on a pattern `Name<*>`. It is not valid on `class *;`, on `class : Base;`, on `ns.*` or on a script class. The attribute covers the named type only. A subclass does not get it.
- The name must fit the stub. `Name` names a class (or a typedef). `Name<*>` names a class template that has type parameters only. `Name<*>` writes a partial specialization with a pack of type parameters. The translator reads the headers and reports an error at the stub in these cases:
  - `Name` is a class template: write `Name<*>`.
  - `Name<*>` is not a class template: write `Name`.
  - `Name` or `Name<*>` is an alias template, or a template with a non-type parameter (for example `my::Fixed<T, N>`) or a template template parameter. A stub cannot name it. For an array type, use the module attribute `array`.
- The module header gets the specialization only when the headers of the script declare the name. A stub for an undeclared type does nothing. A namespace that is `inline` in the header ("__1" of libc++) is not part of the name. A namespace that is not inline, such as `std::__detail`, is part of it.
- `std.` in the stub follows the std rule: `[native, checked_index] class std.deque<*>;` names `deque` of the configured namespace.
- An rvalue that is a checked container returns the element by value (moved), as `hot::deref` does. For a proxy, such as the element of an rvalue `std::vector<bool>`, it returns the `value_type` by value, so no proxy dangles.

The translator writes this for a stub in the module header:

```cpp
namespace hot { template <class... A> struct checked_index< ::my::Ring<A...>> : std::true_type {}; }
```

A host can write the specialization by hand, for example for a type that a `.cpp` script uses. A `.cpp` script has raw bodies and calls `operator[]` itself. A hand-written `std::false_type` specialization removes the default check of a type. Do not combine a stub with a hand-written `hot::checked_index` specialization for the same type: the two definitions are a redefinition in one translation unit.

Put the `checked_index` stubs in a shared prelude. Then every module of one program writes the same trait definition. A module that uses the type without the stub can have another trait value. In an AOT program, the modules are compiled into one program. A module without the stub can then differ from a module with it, and the one-definition rule is broken for that type.

The macro `HOT_RUNTIME_CHECKS=0` (CMake option `HOT_RUNTIME_CHECKS=OFF`) removes the test of every checked type. Then an index outside the container is undefined behavior again, and `hot::index` costs nothing. The trait `hot::checked_index` is separate from `hot::holds_no_code`. A checked type that is not trivially copyable still needs a `holds_no_code` specialization if it cannot hold code.

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

The array template is checked. If the template has the form `Name<$T, $N>` (a plain template-id), the translator writes `template <class T, auto N> struct checked_index< ::Name<T, N>> : std::true_type {};` in the module header, when the headers declare `Name`. The type needs `size()` and `operator[]`. `Name` can be an alias template that passes `T` and `N` straight to the real template, as in `template <class T, std::size_t N> using FixedArray = std::array<T, N>;`. An alias that changes `T` or `N` cannot be deduced, and the C++ compiler rejects the specialization: use the real array type in the module attribute. For another form, such as a nested type `Name<$T, $N>::type`, or the order `Name<$N, $T>`, add a stub `[native, checked_index]` for the real array type (see "Bounds check on an index"). Without it, the type has no check.

The host should also specialize `hot::holds_no_code` for the custom array type, as the runtime does for `std::array`. Without it, a custom array type that is not trivially copyable counts as a type that can hold code. The two traits are separate.

A stub in a prelude can name an array type only in the base of a stub such as `class : Wrap<int[3]>;`. That type uses the template of the earlier preludes and of the same prelude.

## Other limits

- Write `.`, not `::` or `->`.
- `T(args)` for a native aggregate needs C++20. In a C++17 build, the native type needs a constructor.
- A native array has no `.` access.
- A native static data member, such as `Vec3.Zero.x`, needs a typed local first.
- A function that the script calls must be exported by the host. Link with `ENABLE_EXPORTS`.
