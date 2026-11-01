// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Case: string concat. The .hot script uses interpolation; C++ uses std::to_string.
// The .cpp twin of string.hot: the same work as the .hot script.
#include "BenchEngine.h"

#include <cstdint>
#include <string>

class BenchStringCpp : public BenchBase
{
public:
    int64_t Run(int n) override
    {
        int64_t s = 0;
        for (int i = 0; i < n; i++)
        {
            std::string t = "v=" + std::to_string(i) + "-" + std::to_string(s);
            s += static_cast<int64_t>(t.size());
        }
        return s;
    }
};
