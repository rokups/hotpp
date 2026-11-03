---
layout: default
title: Module attributes and other limits
nav_order: 19
group: Hot script
---

## Module attributes

A module attribute list is an attribute list that ends with `;` and has no item.

```
[std("eastl")];
```

The attributes apply to the module (the file) that holds the list. A bare `;` is valid and does nothing.

The attribute `std("name")` sets the name of the standard namespace. The default is `std`. A host that uses another library, such as EASTL, sets it one time in a prelude. The generated C++ then writes `eastl::string`, `eastl::vector`, `eastl::shared_ptr` and so on.

- A list in a prelude is the default for each module of the host.
- A list in a script overrides the default for that script only.
- One attribute name is valid one time in one file. A repeat is an error. The attribute `lib` is an exception (see "Library namespaces").
- The preludes are an ordered list. A later prelude overrides the `std` and `array` of an earlier prelude. The attribute `lib` adds up.
- An imported script uses the namespace of the main script. It can set only that same namespace.

In a script, `std.x` is the name `x` in the configured namespace. A script that writes `std.string` gets `eastl::string`.

With a namespace other than `std`, the script cannot name the real `std`. The name `std.x` always means `x` in the configured namespace. To use a second library next to the real `std`, use the attribute `lib` instead (see "Library namespaces").

An interpolated string does not follow the configured namespace. It uses `std::format` or `fmt::format` (the `--format` option) and gives a `std::string`.

A string in a stub can use `$std::` for the configured namespace:

```
[std("eastl")];
[native, default_handle("$std::shared_ptr<$T>")] class *;
```

A host gives its preludes to the translator in `hotc::TranslateOptions::preludes`. A run-time host gives them in `hot::DefaultBackendOptions::preludes`, as a list of name and text. When a prelude file changes, the host calls `Backend::set_preludes` with the new list and loads its scripts again. It must not call it during a load.

Plain `std::` in a stub string stays as you write it. The names of `<type_traits>`, `size_t` and the C functions always use `std::`.

The host must declare the namespace. For a test, `namespace mystd = std;` is enough. Use the same namespace in the C++ text that `libclang` reads.

### Library namespaces

The attribute `lib("name")` declares a standard-like library namespace, for example EASTL. The standard namespace does not change. So a script uses the two libraries side by side:

```
[lib("eastl")];
```

```
std.vector<int> a = std.vector<int>();     // std::vector<int>
eastl.vector<int> b = eastl.vector<int>(); // eastl::vector<int>
```

- A script names a type of the library with its namespace: `eastl.vector<int>`. A namespace alias such as `ea` also names the same types. Put the real namespace in `lib`.
- The attribute can come more than one time, also in one list: `[lib("eastl"), lib("other")];`. The preludes, the script and its imports add to one set.
- The attribute is valid in a module attribute list only. `lib("std")` is an error.

The translator gives each library namespace `L` the traits that the runtime has for std. The module header gets a trait only when the headers declare the name with the expected kind of template:

- `L::vector` and `L::array<T, N>` have the bounds check of an index (`hot::checked_index`), as `std::vector` and `std::array` have.
- `L::vector`, `L::array`, `L::basic_string`, `L::map` and `L::unordered_map` are free of code when their elements are (`hot::holds_no_code`). The allocator, the hash and the order of these containers count as free of code. So a field of such a type does not block the unload of an old generation.
- A stub handle template `L::shared_ptr<$T>` gets the weak handle `L::weak_ptr<T>` for `[weak]`, and `new` uses `L::make_shared`.

The other parts of a script do not depend on the namespace: `foreach`, an index, a member call, a string literal for a parameter or a field, and `using` of a header.

Do not write a `hot::checked_index` or `hot::holds_no_code` specialization by hand for these types: the two definitions are a redefinition. A stub `[native, checked_index] class eastl.vector<*>;` is permitted, because it gives the same text.

Limits:

- An interpolated string `"${x}"` is always a `std::string`. For a string of the library, write `eastl.string s = "${x}".c_str();`.
- A script class without a native base uses the `shared_ptr` of the standard namespace (the attribute `std`).
- `this` as a handle needs `enable_shared_from_this` in the shell. The translator adds it only for the `shared_ptr` of the standard namespace.
- A fixed-size array `T[N]` uses the array template (the attribute `array`), not `L::array`.
- The traits are for the names above only. For another container, add a stub with `checked_index`, or write a `hot::holds_no_code` specialization in the host.

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

## Script attributes

A script attribute is a tag on a script class. The host reads the tags at run time. For example, a host can register each class with a tag in its own type system.

Declare an attribute class with `[attribute] class Name;`. Then put the name in the attribute list of a script class:

```hot
[attribute] class Spawnable;
[attribute] class Saved;

[Spawnable, Saved]
class Enemy : Actor {
}
```

- An attribute class has a plain name and no body. It takes no other attribute.
- A prelude, the script or an imported script can declare it. A prelude declaration makes the tag available to all scripts of the host.
- A tag has no arguments. A tag with no declaration is an error: `unknown attribute 'Name' on class 'C'`.
- A tag applies to its class only. A child class does not get the tags of its parent.
- Only a script class takes a tag. A stub, a module variable and a module attribute list do not.

A `.cpp` script puts the macro `HOT_ATTRIBUTE("Name")` after `class`. The macro is in `hot/script.h`:

```cpp
class HOT_ATTRIBUTE("Spawnable") Enemy : public Actor {
};
```

- The macro is the annotation `[[clang::annotate("hot.attribute=Name")]]`. Only clang keeps it.
- A `.cpp` script cannot declare an attribute class. A prelude must declare it.

At run time, `Loader::stats().classes` gives one `hot::ClassInfo` for each class. Its field `attributes` holds the tag names in source order. The record field `ClassRecord::attributes` holds the same names as a list that ends with `nullptr`. A reload replaces the tags with the tags of the new generation. A removed class has no tags.

## `[nonnull]`

A `[nonnull]` stub tells the analyzer that a native return of that class is not null. `[nonnull] class *;` sets the default for all stubs. The attribute works on a whole class only. A stub has no members, so `[nonnull]` on a method return, a parameter or a property is not possible in a stub. The members of a native class come from the C++ header (libclang). A contract for a member needs a mark in the header, for example `[[clang::annotate(...)]]`. The frontend already reads annotations for script attributes. This is not done yet.

## Other limits

- Write `.`, not `::` or `->`.
- `T(args)` for a native aggregate needs C++20. In a C++17 build, the native type needs a constructor.
- A native array has no `.` access.
- A native static data member, such as `Vec3.Zero.x`, needs a typed local first.
- A function that the script calls must be exported by the host. Link with `ENABLE_EXPORTS`.
