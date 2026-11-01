// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Case: std::vector access loop.
// The .cpp twin of vector.hot: the same work as the .hot script.
#include "BenchEngine.h"

#include <cstdint>
#include <vector>

class BenchVectorCpp : public BenchBase
{
public:
    std::vector<int> v;

    int64_t Run(int n) override
    {
        if (v.size() == 0)
        {
            for (int j = 0; j < 256; j++)
                v.push_back(j);
        }
        int64_t s = 0;
        for (int i = 0; i < n; i++)
        {
            int k = i & 255;
            v[k] = (v[(k + 1) & 255] + i) & 65535;
            s += v[k];
        }
        return s;
    }
};
