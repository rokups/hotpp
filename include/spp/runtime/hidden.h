// The SPP_HIDDEN macro. Helpers on the script call path must not create debugger frames or variables.
#pragma once

// A small helper that the generated code calls. It inlines at the call site and has no debug
// info of its own, so "step into" on a script line does not enter it.
#if defined(__clang__) || defined(__GNUC__)
    #define SPP_HIDDEN [[gnu::always_inline, gnu::nodebug]] inline
#elif defined(_MSC_VER)
    #define SPP_HIDDEN __forceinline
#else
    #define SPP_HIDDEN inline
#endif

// A helper that cannot be always_inline (large, out-of-line or address taken). It has no debug
// info, so the debugger steps over it. Put it before the return type or on the declaration.
#if defined(__clang__) || defined(__GNUC__)
    #define SPP_NODEBUG [[gnu::nodebug]]
#else
    #define SPP_NODEBUG
#endif

// An attribute list for a local variable in generated code. The variable can be unused. It has
// no debug info, so the debugger does not show it.
// unverified: not built on MSVC. The MSVC form has no nodebug attribute.
#if defined(__clang__) || defined(__GNUC__)
    #define SPP_NODEBUG_LOCAL [[maybe_unused, gnu::nodebug]]
#else
    #define SPP_NODEBUG_LOCAL [[maybe_unused]]
#endif
