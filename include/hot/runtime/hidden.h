// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The HOT_HIDDEN macro. Helpers on the script call path must not create debugger frames or variables.
#pragma once

#include "hot/runtime/export.h"

// A small helper that the generated code calls. It inlines at the call site and has no debug
// info of its own, so "step into" on a script line does not enter it.
// Only clang knows the nodebug attribute. GCC ignores it and warns.
#if defined(__clang__)
    #define HOT_HIDDEN [[gnu::always_inline, gnu::nodebug]] inline
#elif defined(__GNUC__)
    #define HOT_HIDDEN [[gnu::always_inline]] inline
#elif defined(_MSC_VER)
    // Not __forceinline: cl does not inline a function with a try block, and then it writes
    // C4714. cl has no nodebug, so __forceinline gives no debugger gain. The optimizer still
    // inlines the small helpers.
    #define HOT_HIDDEN inline
#else
    #define HOT_HIDDEN inline
#endif

// A helper that cannot be always_inline (large, out-of-line or address taken). It has no debug
// info, so the debugger steps over it. Put it before the return type or on the declaration.
#if defined(__clang__)
    #define HOT_NODEBUG [[gnu::nodebug]]
#else
    #define HOT_NODEBUG
#endif

// A cold helper of a fast path. It stays out of line, so the fast path keeps few registers and
// no stack slots. It has no debug info. Put it before the return type.
#if defined(__clang__)
    #define HOT_NOINLINE [[gnu::noinline, gnu::nodebug]]
#elif defined(__GNUC__)
    #define HOT_NOINLINE [[gnu::noinline]]
#elif defined(_MSC_VER)
    #define HOT_NOINLINE __declspec(noinline)
#else
    #define HOT_NOINLINE
#endif

// An attribute list for a local variable in generated code. The variable can be unused. It has
// no debug info, so the debugger does not show it.
#if defined(__clang__)
    #define HOT_NODEBUG_LOCAL [[maybe_unused, gnu::nodebug]]
#else
    #define HOT_NODEBUG_LOCAL [[maybe_unused]]
#endif
