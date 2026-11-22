// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// A test invoker for the module attribute [fn_invoker("TEST_INVOKER")]. The macro makes a plain
// function pointer from the pointer of a free function. A function with arithmetic parameters and
// an arithmetic or void result gets the argument "in" for each parameter. Any other signature
// gives a pointer that returns false. The header has no code that the host must export.
#pragma once

#include <type_traits>

struct TestInvokeArgs
{
    int in = 0;
    mutable int out = 0; // the result: the invoker has a const argument
};

using TestInvoker = bool (*)(const TestInvokeArgs&);

namespace test_invoke
{
template <class R, class... P> bool run(R (*fn)(P...), const TestInvokeArgs& a)
{
    if constexpr ((std::is_arithmetic_v<P> && ...) && (std::is_arithmetic_v<R> || std::is_void_v<R>))
    {
        if constexpr (std::is_void_v<R>)
            fn(static_cast<P>(a.in)...);
        else
            a.out = static_cast<int>(fn(static_cast<P>(a.in)...));
        return true;
    }
    else
        return false;
}
} // namespace test_invoke

#define TEST_INVOKER(fn) (+[](const TestInvokeArgs& a) -> bool { return test_invoke::run(&fn, a); })
