// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Case: native to script call. The native method DriveHook calls the virtual Hook n times.
// The .cpp twin of virtual_hook.hot: the same work as the .hot script.
#include "BenchEngine.h"

#include <cstdint>

class BenchHookCpp : public BenchBase
{
public:
    int Hook(int x) override
    {
        return x + 1;
    }

    int64_t Run(int n) override
    {
        return DriveHook(n);
    }
};
