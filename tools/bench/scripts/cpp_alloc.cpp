// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Case: allocation and free of a script object with new and delete.
// The .cpp twin of alloc.hot. A .cpp script owns the object with a raw pointer, not with a handle.
#include "BenchEngine.h"

#include <cstdint>

class BenchObjCpp : public BenchBase
{
public:
    int a = 1;
    int b = 2;
};

class BenchAllocCpp : public BenchBase
{
public:
    int64_t Run(int n) override
    {
        int64_t s = 0;
        for (int i = 0; i < n; i++)
        {
            BenchObjCpp* o = new BenchObjCpp();
            o->a = i;
            s += o->a + o->b;
            delete o;
        }
        return s;
    }
};
