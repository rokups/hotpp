// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Case: script to script call. Fib is recursive, so the compiler cannot remove the calls.
// The .cpp twin of call_script.hot: the same work as the .hot script.
#include "BenchEngine.h"

#include <cstdint>

class BenchCallScriptCpp : public BenchBase
{
public:
    int Fib(int n)
    {
        if (n < 2)
            return n;
        return Fib(n - 1) + Fib(n - 2);
    }

    int64_t Run(int n) override
    {
        int64_t s = 0;
        for (int i = 0; i < n; i++)
            s += Fib(10 + (i & 1));
        return s;
    }
};
