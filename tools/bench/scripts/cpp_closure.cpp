// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Case: closure call. The lambda captures a local.
// The .cpp twin of closure.hot: the same work as the .hot script.
#include "BenchEngine.h"

#include <cstdint>
#include <functional>

class BenchClosureCpp : public BenchBase
{
public:
    int64_t Run(int n) override
    {
        int k = 3;
        std::function<int(int)> f = [k](int x) { return x + k; };
        int64_t s = 0;
        for (int i = 0; i < n; i++)
            s += f(i);
        return s;
    }
};
