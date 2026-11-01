// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Case: integer arithmetic loop.
// The .cpp twin of arith_int.hot: the same work as the .hot script.
#include "BenchEngine.h"

#include <cstdint>

class BenchArithIntCpp : public BenchBase
{
public:
    int64_t Run(int n) override
    {
        int64_t s = 0;
        int x = 1;
        for (int i = 0; i < n; i++)
        {
            x = (x * 31 + i) & 1048575;
            s += x % 7 + (x >> 3);
        }
        return s;
    }
};
