// The HOT_HIDDEN macro. Helpers on the script call path must not create debugger frames or variables.
#pragma once

// A small helper that the generated code calls. It inlines at the call site and has no debug
// info of its own, so "step into" on a script line does not enter it.
// HOT_EXPORT keeps a symbol visible outside its module. The AOT libraries build with
// -fvisibility=hidden. A type whose RTTI must match across the host and a script module needs it.
#if defined(__clang__) || defined(__GNUC__)
    #define HOT_EXPORT [[gnu::visibility("default")]]
#else
    #define HOT_EXPORT
#endif

#if defined(__clang__) || defined(__GNUC__)
    #define HOT_HIDDEN [[gnu::always_inline, gnu::nodebug]] inline
#elif defined(_MSC_VER)
    #define HOT_HIDDEN __forceinline
#else
    #define HOT_HIDDEN inline
#endif

// A helper that cannot be always_inline (large, out-of-line or address taken). It has no debug
// info, so the debugger steps over it. Put it before the return type or on the declaration.
#if defined(__clang__) || defined(__GNUC__)
    #define HOT_NODEBUG [[gnu::nodebug]]
#else
    #define HOT_NODEBUG
#endif

// A cold helper of a fast path. It stays out of line, so the fast path keeps few registers and
// no stack slots. It has no debug info. Put it before the return type.
#if defined(__clang__) || defined(__GNUC__)
    #define HOT_NOINLINE [[gnu::noinline, gnu::nodebug]]
#elif defined(_MSC_VER)
    #define HOT_NOINLINE __declspec(noinline)
#else
    #define HOT_NOINLINE
#endif

// An attribute list for a local variable in generated code. The variable can be unused. It has
// no debug info, so the debugger does not show it.
// unverified: not built on MSVC. The MSVC form has no nodebug attribute.
#if defined(__clang__) || defined(__GNUC__)
    #define HOT_NODEBUG_LOCAL [[maybe_unused, gnu::nodebug]]
#else
    #define HOT_NODEBUG_LOCAL [[maybe_unused]]
#endif
