---
layout: default
title: operator->, stub checks and index bounds
nav_order: 18
group: Hot script
---

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
- The array type `T[N]` of the script. This includes a custom type from the module attribute `array` (see [The array type](native-interop-attributes.html#the-array-type)).

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
- The module header gets the specialization only when the headers of the script declare the name. A stub for an undeclared type does nothing. A namespace that is `inline` in the header ("`__1`" of libc++) is not part of the name. A namespace that is not inline, such as `std::__detail`, is part of it.
- `std.` in the stub follows the std rule: `[native, checked_index] class std.deque<*>;` names `deque` of the configured namespace.
- A library namespace (the module attribute `lib`, see [Module attributes](native-interop-attributes.html#library-namespaces)) has the check on its `vector` and `array` without a stub.
- An rvalue that is a checked container returns the element by value (moved), as `hot::deref` does. For a proxy, such as the element of an rvalue `std::vector<bool>`, it returns the `value_type` by value, so no proxy dangles.

The translator writes this for a stub in the module header:

```cpp
namespace hot { template <class... A> struct checked_index< ::my::Ring<A...>> : std::true_type {}; }
```

A host can write the specialization by hand, for example for a type that a `.cpp` script uses. A `.cpp` script has raw bodies and calls `operator[]` itself. A hand-written `std::false_type` specialization removes the default check of a type. Do not combine a stub with a hand-written `hot::checked_index` specialization for the same type: the two definitions are a redefinition in one translation unit.

Put the `checked_index` stubs in a shared prelude. Then every module of one program writes the same trait definition. A module that uses the type without the stub can have another trait value. In an AOT program, the modules are compiled into one program. A module without the stub can then differ from a module with it, and the one-definition rule is broken for that type.

The macro `HOT_RUNTIME_CHECKS=0` (CMake option `HOT_RUNTIME_CHECKS=OFF`) removes the test of every checked type. Then an index outside the container is undefined behavior again, and `hot::index` costs nothing. The trait `hot::checked_index` is separate from `hot::holds_no_code`. A checked type that is not trivially copyable still needs a `holds_no_code` specialization if it cannot hold code.
