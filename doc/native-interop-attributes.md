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
