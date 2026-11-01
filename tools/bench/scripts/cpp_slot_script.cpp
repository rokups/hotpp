// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Case: a virtual call from script to script through the base class (slot dispatch).
// The .cpp twin of slot_script.hot. The new virtual method of a script class uses a slot.
#include "BenchEngine.h"

#include <cstdint>

class BenchShapeCpp : public BenchBase
{
public:
    int w = 2;

    virtual int Area(int x)
    {
        return x;
    }
};

class BenchSquareCpp : public BenchShapeCpp
{
public:
    int Area(int x) override
    {
        return x * w + 1;
    }
};

class BenchSlotScriptCpp : public BenchBase
{
public:
    BenchShapeCpp* s = nullptr;

    ~BenchSlotScriptCpp()
    {
        delete s;
    }

    int64_t Run(int n) override
    {
        if (s == nullptr)
            s = new BenchSquareCpp();
        int64_t t = 0;
        for (int i = 0; i < n; i++)
            t += s->Area(i & 255);
        return t;
    }
};
